#include <iostream>
#include <vector>
#include <queue>
#include <set>
#include <map>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <random>
#include <atomic>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <fstream>

using namespace std;

// 电梯状态枚举
enum class ElevatorState {
    IDLE,
    MOVING_UP,
    MOVING_DOWN,
    DOORS_OPEN,
    EMERGENCY_STOP,
    MAINTENANCE
};

// 电梯请求类型枚举
enum class RequestType {
    INTERNAL,    // 电梯内部按钮
    EXTERNAL_UP, // 外部上行按钮
    EXTERNAL_DOWN // 外部下行按钮
};

// 电梯请求结构
struct ElevatorRequest {
    int floor;
    time_t timestamp;
    RequestType type;
    bool isEmergency;
    
    ElevatorRequest(int f, RequestType t = RequestType::INTERNAL, bool emergency = false) 
        : floor(f), timestamp(time(nullptr)), type(t), isEmergency(emergency) {}
};

// 电梯类
class Elevator {
private:
    int id;
    int currentFloor;
    ElevatorState state;
    int maxFloors;
    int capacity;
    int currentPassengers;
    bool doorOpen;
    bool overloaded;
    set<int> internalRequests;  // 内部按钮请求
    map<int, pair<bool, bool>> externalRequests; // 外部请求: floor -> (upPressed, downPressed)
    mutex mtx;
    condition_variable cv;
    atomic<bool> running;
    atomic<bool> emergencyStop;
    atomic<bool> maintenanceMode;
    string logFile;
    
    // 统计信息
    int totalTrips;
    int totalFloorsTraveled;
    time_t startTime;
    time_t lastMaintenance;
    
    void logEvent(const string& event) {
        ofstream log(logFile, ios::app);
        if (log.is_open()) {
            time_t now = time(nullptr);
            char timeStr[100];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
            log << "[" << timeStr << "] 电梯 " << id << ": " << event << endl;
        }
    }
    
public:
    Elevator(int id, int maxFloors, int capacity, const string& logFilename = "") 
        : id(id), currentFloor(1), state(ElevatorState::IDLE), maxFloors(maxFloors), 
          capacity(capacity), currentPassengers(0), doorOpen(false), overloaded(false),
          running(true), emergencyStop(false), maintenanceMode(false),
          totalTrips(0), totalFloorsTraveled(0), startTime(time(nullptr)), lastMaintenance(time(nullptr)) {
        
        if (logFilename.empty()) {
            ostringstream ss;
            ss << "elevator_" << id << ".log";
            logFile = ss.str();
        } else {
            logFile = logFilename;
        }
        
        // 清空日志文件
        ofstream log(logFile, ios::trunc);
        log << "电梯 " << id << " 日志开始" << endl;
    }

    void start() {
        thread controlThread(&Elevator::control, this);
        controlThread.detach();
    }

    void stop() {
        running = false;
        cv.notify_all();
    }

    bool requestFloor(int floor, RequestType type = RequestType::INTERNAL, bool emergency = false) {
        if (floor < 1 || floor > maxFloors) {
            cout << "电梯 " << id << ": 无效楼层 " << floor << endl;
            return false;
        }

        if (maintenanceMode && !emergency) {
            cout << "电梯 " << id << ": 维护模式中，不接受新请求" << endl;
            return false;
        }

        if (emergency) {
            emergencyStop = true;
            cout << "电梯 " << id << ": 紧急停止请求!" << endl;
            logEvent("紧急停止请求");
            cv.notify_one();
            return true;
        }

        lock_guard<mutex> lock(mtx);
        
        if (type == RequestType::INTERNAL) {
            if (internalRequests.find(floor) == internalRequests.end()) {
                internalRequests.insert(floor);
                cout << "电梯 " << id << ": 收到内部请求 " << floor << "楼" << endl;
                logEvent("收到内部请求 " + to_string(floor) + "楼");
                cv.notify_one();
                return true;
            }
        } else {
            // 外部请求
            if (externalRequests.find(floor) == externalRequests.end()) {
                externalRequests[floor] = make_pair(false, false);
            }
            
            if (type == RequestType::EXTERNAL_UP) {
                externalRequests[floor].first = true;
                cout << "电梯 " << id << ": 收到外部上行请求 " << floor << "楼" << endl;
                logEvent("收到外部上行请求 " + to_string(floor) + "楼");
            } else {
                externalRequests[floor].second = true;
                cout << "电梯 " << id << ": 收到外部下行请求 " << floor << "楼" << endl;
                logEvent("收到外部下行请求 " + to_string(floor) + "楼");
            }
            
            cv.notify_one();
            return true;
        }
        return false;
    }

    void control() {
        while (running) {
            // 检查紧急停止
            if (emergencyStop) {
                handleEmergency();
                continue;
            }
            
            // 检查维护模式
            if (maintenanceMode) {
                handleMaintenance();
                continue;
            }

            unique_lock<mutex> lock(mtx);
            cv.wait(lock, [this] { 
                return (!internalRequests.empty() || hasExternalRequests() || !running || emergencyStop || maintenanceMode); 
            });

            if (!running) break;
            if (emergencyStop || maintenanceMode) continue;

            // 决定方向
            if (state == ElevatorState::IDLE) {
                if (!internalRequests.empty() || hasExternalRequests()) {
                    int nextFloor = findNextFloor();
                    state = (nextFloor > currentFloor) ? ElevatorState::MOVING_UP : ElevatorState::MOVING_DOWN;
                }
            }

            lock.unlock();

            // 移动电梯
            move();

            lock.lock();
            // 检查是否有请求在当前楼层
            if (shouldStopAtCurrentFloor()) {
                openDoors();
                processStop();
                closeDoors();
            }

            // 更新状态
            updateState();
        }
    }
    
    bool hasExternalRequests() const {
        for (const auto& req : externalRequests) {
            if (req.second.first || req.second.second) {
                return true;
            }
        }
        return false;
    }
    
    int findNextFloor() {
        // 优先处理内部请求
        if (!internalRequests.empty()) {
            if (state == ElevatorState::MOVING_UP) {
                auto it = internalRequests.upper_bound(currentFloor);
                if (it != internalRequests.end()) return *it;
            } else if (state == ElevatorState::MOVING_DOWN) {
                auto it = internalRequests.lower_bound(currentFloor);
                if (it != internalRequests.begin()) return *(--it);
            }
            return *internalRequests.begin();
        }
        
        // 处理外部请求
        int closestFloor = -1;
        int minDistance = maxFloors + 1;
        
        for (const auto& req : externalRequests) {
            if ((req.second.first && req.first >= currentFloor) || 
                (req.second.second && req.first <= currentFloor)) {
                int distance = abs(req.first - currentFloor);
                if (distance < minDistance) {
                    minDistance = distance;
                    closestFloor = req.first;
                }
            }
        }
        
        if (closestFloor != -1) return closestFloor;
        
        // 如果没有顺路请求，找最近的请求
        for (const auto& req : externalRequests) {
            if (req.second.first || req.second.second) {
                int distance = abs(req.first - currentFloor);
                if (distance < minDistance) {
                    minDistance = distance;
                    closestFloor = req.first;
                }
            }
        }
        
        return closestFloor;
    }
    
    bool shouldStopAtCurrentFloor() {
        // 检查内部请求
        if (internalRequests.find(currentFloor) != internalRequests.end()) {
            return true;
        }
        
        // 检查外部请求
        auto it = externalRequests.find(currentFloor);
        if (it != externalRequests.end()) {
            if (state == ElevatorState::MOVING_UP && it->second.first) {
                return true;
            }
            if (state == ElevatorState::MOVING_DOWN && it->second.second) {
                return true;
            }
            if (state == ElevatorState::IDLE && (it->second.first || it->second.second)) {
                return true;
            }
        }
        
        return false;
    }
    
    void processStop() {
        // 移除内部请求
        internalRequests.erase(currentFloor);
        
        // 移除外部请求
        auto it = externalRequests.find(currentFloor);
        if (it != externalRequests.end()) {
            if (state == ElevatorState::MOVING_UP) {
                it->second.first = false;
            } else if (state == ElevatorState::MOVING_DOWN) {
                it->second.second = false;
            } else {
                it->second.first = false;
                it->second.second = false;
            }
            
            // 如果没有请求了，移除该楼层
            if (!it->second.first && !it->second.second) {
                externalRequests.erase(it);
            }
        }
        
        // 更新统计信息
        totalTrips++;
    }

    void move() {
        if (state != ElevatorState::MOVING_UP && state != ElevatorState::MOVING_DOWN) 
            return;

        // 模拟移动时间
        this_thread::sleep_for(chrono::seconds(1));
        
        int oldFloor = currentFloor;
        
        if (state == ElevatorState::MOVING_UP) {
            currentFloor++;
        } else if (state == ElevatorState::MOVING_DOWN) {
            currentFloor--;
        }
        
        totalFloorsTraveled += abs(currentFloor - oldFloor);
        
        cout << "电梯 " << id << ": 到达 " << currentFloor << "楼" << endl;
        logEvent("到达 " + to_string(currentFloor) + "楼");
    }

    void openDoors() {
        state = ElevatorState::DOORS_OPEN;
        doorOpen = true;
        cout << "电梯 " << id << ": 门在 " << currentFloor << " 楼打开" << endl;
        logEvent("门在 " + to_string(currentFloor) + " 楼打开");
        
        // 模拟乘客进出
        simulatePassengers();
        
        this_thread::sleep_for(chrono::seconds(2));
    }

    void closeDoors() {
        // 检查是否超载
        if (overloaded) {
            cout << "电梯 " << id << ": 超载警告! 请减少乘客数量" << endl;
            logEvent("超载警告");
            this_thread::sleep_for(chrono::seconds(3));
            overloaded = false;
        }
        
        cout << "电梯 " << id << ": 门关闭" << endl;
        logEvent("门关闭");
        doorOpen = false;
        this_thread::sleep_for(chrono::seconds(1));
    }

    void simulatePassengers() {
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> enterDis(0, 5);
        uniform_int_distribution<> exitDis(0, min(5, currentPassengers));
        
        int entering = enterDis(gen);
        int exiting = exitDis(gen);
        
        // 确保不超过容量
        entering = min(entering, capacity - currentPassengers);
        
        currentPassengers += entering - exiting;
        
        // 检查是否超载
        if (currentPassengers > capacity) {
            overloaded = true;
            currentPassengers = capacity; // 强制减少到容量限制
        }
        
        cout << "电梯 " << id << ": " << entering << "人进入, " << exiting << "人离开, 当前乘客: " << currentPassengers << "/" << capacity << endl;
        logEvent(to_string(entering) + "人进入, " + to_string(exiting) + "人离开, 当前乘客: " + 
                 to_string(currentPassengers) + "/" + to_string(capacity));
    }

    void updateState() {
        if (internalRequests.empty() && !hasExternalRequests()) {
            state = ElevatorState::IDLE;
        } else {
            // 决定下一步方向
            int nextFloor = findNextFloor();
            if (nextFloor != -1) {
                state = (nextFloor > currentFloor) ? ElevatorState::MOVING_UP : ElevatorState::MOVING_DOWN;
            } else {
                state = ElevatorState::IDLE;
            }
        }
    }

    void handleEmergency() {
        cout << "电梯 " << id << ": 紧急停止已激活!" << endl;
        logEvent("紧急停止已激活");
        state = ElevatorState::EMERGENCY_STOP;
        
        // 立即开门如果在楼层上
        if (currentFloor >= 1 && currentFloor <= maxFloors) {
            doorOpen = true;
            cout << "电梯 " << id << ": 紧急开门在 " << currentFloor << " 楼" << endl;
            logEvent("紧急开门在 " + to_string(currentFloor) + " 楼");
        }
        
        // 等待紧急情况解除
        while (emergencyStop && running) {
            this_thread::sleep_for(chrono::seconds(1));
        }
        
        if (running) {
            cout << "电梯 " << id << ": 紧急情况解除，恢复正常运行" << endl;
            logEvent("紧急情况解除，恢复正常运行");
            state = ElevatorState::IDLE;
            doorOpen = false;
        }
    }
    
    void handleMaintenance() {
        cout << "电梯 " << id << ": 维护模式中..." << endl;
        state = ElevatorState::MAINTENANCE;
        
        // 等待维护模式结束
        while (maintenanceMode && running) {
            this_thread::sleep_for(chrono::seconds(1));
        }
        
        if (running) {
            cout << "电梯 " << id << ": 维护模式结束，恢复正常运行" << endl;
            logEvent("维护模式结束，恢复正常运行");
            state = ElevatorState::IDLE;
            lastMaintenance = time(nullptr);
        }
    }

    void resetEmergency() {
        emergencyStop = false;
        cv.notify_one();
    }
    
    void setMaintenanceMode(bool mode) {
        maintenanceMode = mode;
        if (!mode) {
            cv.notify_one();
        }
    }

    // 获取电梯信息
    int getId() const { return id; }
    int getCurrentFloor() const { return currentFloor; }
    ElevatorState getState() const { return state; }
    int getPassengerCount() const { return currentPassengers; }
    int getCapacity() const { return capacity; }
    set<int> getInternalRequests() const { 
        lock_guard<mutex> lock(mtx);
        return internalRequests; 
    }
    
    map<int, pair<bool, bool>> getExternalRequests() const {
        lock_guard<mutex> lock(mtx);
        return externalRequests;
    }
    
    bool isFull() const { return currentPassengers >= capacity; }
    bool isEmergency() const { return emergencyStop; }
    bool isInMaintenance() const { return maintenanceMode; }
    
    string getStateString() const {
        switch (state) {
            case ElevatorState::IDLE: return "空闲";
            case ElevatorState::MOVING_UP: return "上行";
            case ElevatorState::MOVING_DOWN: return "下行";
            case ElevatorState::DOORS_OPEN: return "门开";
            case ElevatorState::EMERGENCY_STOP: return "紧急停止";
            case ElevatorState::MAINTENANCE: return "维护中";
            default: return "未知";
        }
    }
    
    void printStatistics() const {
        time_t now = time(nullptr);
        double hours = difftime(now, startTime) / 3600.0;
        int floorsPerHour = hours > 0 ? totalFloorsTraveled / hours : 0;
        
        cout << "电梯 " << id << " 统计信息:" << endl;
        cout << "  运行时间: " << fixed << setprecision(1) << hours << " 小时" << endl;
        cout << "  总行程数: " << totalTrips << endl;
        cout << "  总行驶楼层: " << totalFloorsTraveled << endl;
        cout << "  平均行驶楼层/小时: " << floorsPerHour << endl;
        
        time_t lastMaintenanceTime = lastMaintenance;
        char timeStr[100];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&lastMaintenanceTime));
        cout << "  上次维护时间: " << timeStr << endl;
    }
};

// 电梯控制系统类
class ElevatorControlSystem {
private:
    vector<Elevator> elevators;
    mutex mtx;
    atomic<bool> running;
    int maxFloors;
    string logDir;
    
public:
    ElevatorControlSystem(int numElevators, int maxFloors, int capacity, const string& logDirectory = "logs") 
        : maxFloors(maxFloors), running(true), logDir(logDirectory) {
        
        // 创建日志目录
        system(("mkdir -p " + logDir).c_str());
        
        for (int i = 0; i < numElevators; i++) {
            string logFile = logDir + "/elevator_" + to_string(i+1) + ".log";
            elevators.emplace_back(i + 1, maxFloors, capacity, logFile);
        }
    }

    void start() {
        for (auto& elevator : elevators) {
            elevator.start();
        }
        
        thread monitorThread(&ElevatorControlSystem::monitor, this);
        monitorThread.detach();
    }

    void stop() {
        running = false;
        for (auto& elevator : elevators) {
            elevator.stop();
        }
    }

    void requestElevator(int floor, RequestType type = RequestType::INTERNAL, bool emergency = false, int preferredElevator = -1) {
        if (floor < 1 || floor > maxFloors) {
            cout << "无效楼层: " << floor << endl;
            return;
        }

        if (emergency) {
            // 紧急情况：通知所有电梯
            for (auto& elevator : elevators) {
                elevator.requestFloor(floor, type, true);
            }
            return;
        }

        if (preferredElevator > 0 && preferredElevator <= elevators.size()) {
            // 指定电梯
            elevators[preferredElevator-1].requestFloor(floor, type);
            cout << "分配请求 " << floor << "楼 给电梯 " << preferredElevator << endl;
            return;
        }

        // 选择最合适的电梯
        int bestElevator = findBestElevator(floor, type);
        elevators[bestElevator].requestFloor(floor, type);
        
        cout << "分配请求 " << floor << "楼 给电梯 " << (bestElevator + 1) << endl;
    }

    int findBestElevator(int floor, RequestType type) {
        int bestIndex = 0;
        int bestScore = INT_MAX;

        for (int i = 0; i < elevators.size(); i++) {
            int score = calculateElevatorScore(i, floor, type);
            if (score < bestScore) {
                bestScore = score;
                bestIndex = i;
            }
        }

        return bestIndex;
    }

    int calculateElevatorScore(int elevatorIndex, int targetFloor, RequestType type) {
        const Elevator& elevator = elevators[elevatorIndex];
        
        // 如果电梯处于紧急状态或维护模式，不使用它
        if (elevator.isEmergency() || elevator.isInMaintenance()) {
            return INT_MAX;
        }
        
        int currentFloor = elevator.getCurrentFloor();
        ElevatorState state = elevator.getState();
        int passengerCount = elevator.getPassengerCount();
        int capacity = elevator.getCapacity();
        
        // 计算距离分数
        int distance = abs(currentFloor - targetFloor);
        
        // 计算方向分数
        int directionScore = 0;
        if (state == ElevatorState::MOVING_UP) {
            if (currentFloor <= targetFloor) {
                directionScore = -10; // 同方向且正在接近
            } else {
                directionScore = 10; // 反方向
            }
        } else if (state == ElevatorState::MOVING_DOWN) {
            if (currentFloor >= targetFloor) {
                directionScore = -10; // 同方向且正在接近
            } else {
                directionScore = 10; // 反方向
            }
        } else if (state == ElevatorState::IDLE) {
            directionScore = -5; // 空闲电梯
        }
        
        // 计算负载分数
        int loadScore = (passengerCount * 10) / capacity;
        
        // 计算类型适配分数
        int typeScore = 0;
        if (type == RequestType::EXTERNAL_UP && state == ElevatorState::MOVING_DOWN) {
            typeScore = 5; // 上行请求但电梯下行，稍微惩罚
        } else if (type == RequestType::EXTERNAL_DOWN && state == ElevatorState::MOVING_UP) {
            typeScore = 5; // 下行请求但电梯上行，稍微惩罚
        }
        
        // 总分数 = 距离 + 方向分数 + 负载分数 + 类型适配分数
        return distance + directionScore + loadScore + typeScore;
    }

    void monitor() {
        while (running) {
            this_thread::sleep_for(chrono::seconds(10));
            printStatus();
            
            // 每5分钟保存一次统计信息
            static int counter = 0;
            if (++counter % 30 == 0) { // 10s * 30 = 300s = 5min
                saveStatistics();
            }
        }
    }
    
    void saveStatistics() {
        ofstream statsFile(logDir + "/statistics.txt", ios::trunc);
        if (statsFile.is_open()) {
            time_t now = time(nullptr);
            char timeStr[100];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
            
            statsFile << "电梯系统统计信息 - " << timeStr << endl;
            statsFile << "==========================================" << endl;
            
            for (const auto& elevator : elevators) {
                time_t startTime = time(nullptr); // 这里应该使用电梯的实际开始时间
                double hours = difftime(now, startTime) / 3600.0;
                
                statsFile << "电梯 " << elevator.getId() << ":" << endl;
                statsFile << "  运行时间: " << fixed << setprecision(1) << hours << " 小时" << endl;
                // 这里可以添加更多统计信息
                statsFile << endl;
            }
        }
    }

    void printStatus() {
        cout << "\n===== 电梯状态监控 =====" << endl;
        cout << "时间: ";
        time_t now = time(nullptr);
        char timeStr[100];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
        cout << timeStr << endl;
        
        for (const auto& elevator : elevators) {
            cout << "电梯 " << elevator.getId() << ": ";
            cout << "楼层 " << elevator.getCurrentFloor() << ", ";
            cout << elevator.getStateString() << ", ";
            cout << "乘客: " << elevator.getPassengerCount() << "/" << elevator.getCapacity();
            
            if (elevator.isEmergency()) {
                cout << ", 紧急状态";
            }
            
            if (elevator.isInMaintenance()) {
                cout << ", 维护模式";
            }
            
            auto internalReqs = elevator.getInternalRequests();
            if (!internalReqs.empty()) {
                cout << ", 内部请求: ";
                for (int floor : internalReqs) {
                    cout << floor << " ";
                }
            }
            
            auto externalReqs = elevator.getExternalRequests();
            if (!externalReqs.empty()) {
                cout << ", 外部请求: ";
                for (const auto& req : externalReqs) {
                    if (req.second.first) cout << req.first << "↑ ";
                    if (req.second.second) cout << req.first << "↓ ";
                }
            }
            
            cout << endl;
        }
        cout << "=======================\n" << endl;
    }

    void resetEmergency(int elevatorId) {
        if (elevatorId > 0 && elevatorId <= elevators.size()) {
            elevators[elevatorId - 1].resetEmergency();
            cout << "电梯 " << elevatorId << " 紧急状态已重置" << endl;
        } else {
            cout << "无效的电梯ID" << endl;
        }
    }
    
    void setMaintenanceMode(int elevatorId, bool mode) {
        if (elevatorId > 0 && elevatorId <= elevators.size()) {
            elevators[elevatorId - 1].setMaintenanceMode(mode);
            cout << "电梯 " << elevatorId << " 维护模式" << (mode ? "开启" : "关闭") << endl;
        } else {
            cout << "无效的电梯ID" << endl;
        }
    }
    
    void printStatistics(int elevatorId = -1) const {
        if (elevatorId == -1) {
            for (const auto& elevator : elevators) {
                elevator.printStatistics();
                cout << endl;
            }
        } else if (elevatorId > 0 && elevatorId <= elevators.size()) {
            elevators[elevatorId - 1].printStatistics();
        } else {
            cout << "无效的电梯ID" << endl;
        }
    }
    
    int getElevatorCount() const {
        return elevators.size();
    }
    
    int getMaxFloors() const {
        return maxFloors;
    }
};

// 全局函数：显示帮助信息
void printHelp() {
    cout << "可用命令:" << endl;
    cout << "  [楼层号] - 请求电梯到指定楼层(内部按钮)" << endl;
    cout << "  u[楼层号] - 请求上行电梯到指定楼层(外部上行按钮)" << endl;
    cout << "  d[楼层号] - 请求下行电梯到指定楼层(外部下行按钮)" << endl;
    cout << "  e [楼层号] - 紧急停止请求" << endl;
    cout << "  r [电梯号] - 重置指定电梯的紧急状态" << endl;
    cout << "  m [电梯号] - 切换指定电梯的维护模式" << endl;
    cout << "  s [电梯号] - 显示指定电梯的统计信息(不指定电梯号显示全部)" << endl;
    cout << "  status - 显示电梯状态" << endl;
    cout << "  help - 显示帮助信息" << endl;
    cout << "  0 - 退出程序" << endl;
}

int main() {
    const int NUM_ELEVATORS = 4;
    const int MAX_FLOORS = 25;
    const int ELEVATOR_CAPACITY = 15;
    
    ElevatorControlSystem system(NUM_ELEVATORS, MAX_FLOORS, ELEVATOR_CAPACITY);
    system.start();

    cout << "电梯控制系统启动 (" << NUM_ELEVATORS << "部电梯, " << MAX_FLOORS << "层)" << endl;
    printHelp();

    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> floorDis(1, MAX_FLOORS);
    uniform_int_distribution<> typeDis(0, 2);

    // 生成一些随机请求
    for (int i = 0; i < 15; ++i) {
        int floor = floorDis(gen);
        RequestType type = static_cast<RequestType>(typeDis(gen));
        system.requestElevator(floor, type);
        this_thread::sleep_for(chrono::milliseconds(300));
    }

    string input;
    int value;
    while (true) {
        cout << "请输入命令: ";
        cin >> input;

        if (input == "0") {
            break;
        } else if (input == "status") {
            system.printStatus();
        } else if (input == "e") {
            cin >> value;
            system.requestElevator(value, RequestType::INTERNAL, true);
        } else if (input == "r") {
            cin >> value;
            system.resetEmergency(value);
        } else if (input == "m") {
            cin >> value;
            system.setMaintenanceMode(value, true);
        } else if (input == "s") {
            if (cin.peek() == '\n') {
                system.printStatistics();
            } else {
                cin >> value;
                system.printStatistics(value);
            }
        } else if (input == "help") {
            printHelp();
        } else if (input[0] == 'u' && input.size() > 1) {
            try {
                value = stoi(input.substr(1));
                system.requestElevator(value, RequestType::EXTERNAL_UP);
            } catch (exception& e) {
                cout << "无效命令!" << endl;
            }
        } else if (input[0] == 'd' && input.size() > 1) {
            try {
                value = stoi(input.substr(1));
                system.requestElevator(value, RequestType::EXTERNAL_DOWN);
            } catch (exception& e) {
                cout << "无效命令!" << endl;
            }
        } else {
            try {
                value = stoi(input);
                system.requestElevator(value);
            } catch (exception& e) {
                cout << "无效命令! 输入 'help' 查看帮助" << endl;
            }
        }
    }

    system.stop();
    cout << "程序结束" << endl;
    return 0;
}