#pragma once


/**
 * @brief 协调单 Session 的飞行控制动作与 HoverTest 工作流。
 *
 * 分类：Production。
 * 输入：DataManager 当前状态和控制请求；输出：CommandDispatcher 队列动作。
 * 关键约束：所有实际飞控动作仍经过统一安全门和 MAVLink 命令调度。
 */

#include "CommandDispatcher.h"
#include "DataManager.h"

#include <thread>
#include <atomic>

enum class HoverMissionState
{
    Idle,
    Guided,
    Arming,
    TakingOff,
    Hovering,
    WaitingCommand,
    Failed
};

const char* HoverMissionStateName(HoverMissionState state);


class FlightControl
{

public:


    FlightControl(
        CommandDispatcher& dispatcher,
        DataManager& manager
    );


    // 析构
    ~FlightControl() noexcept;



    //-----------------------------
    // 启动悬停任务
    //-----------------------------

    void StartHoverMission(float targetAltitudeMeters);



    //-----------------------------
    // 停止悬停任务
    //-----------------------------

    void StopHoverMission();



    HoverMissionState GetHoverMissionState() const;
    float GetHoverTargetAltitudeMeters() const;



private:


    //-----------------------------
    // 悬停任务线程函数
    //-----------------------------

    void HoverMission();

    void Takeoff(float altitude);



    //-----------------------------
    // 等待高度
    //-----------------------------

    bool WaitAltitude(
        float target,
        float timeout
    );

    bool WaitMode(
        int targetMode,
        float timeout);

    bool WaitArmed(
        bool targetArmed,
        float timeout);

    void SetHoverMissionState(HoverMissionState state);



private:


    CommandDispatcher& dispatcher;


    DataManager& manager;



    //-----------------------------
    // 工作线程
    //-----------------------------

    std::thread hoverThread;



    //-----------------------------
    // 运行状态
    //-----------------------------

    std::atomic<bool> hoverRunning{ false };
    std::atomic<HoverMissionState> hoverState{ HoverMissionState::Idle };
    std::atomic<float> hoverTargetAltitudeMeters{ 0.0f };


};
