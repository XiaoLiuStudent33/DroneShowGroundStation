#include "FlightControl.h"
#include "ConsoleUI.h"
#include "SafetyConfig.h"

#include <algorithm>
#include <thread>
#include <chrono>

using namespace std;

const char* HoverMissionStateName(const HoverMissionState state)
{
    switch (state)
    {
    case HoverMissionState::Idle: return "Idle";
    case HoverMissionState::Guided: return "Guided";
    case HoverMissionState::Arming: return "Arming";
    case HoverMissionState::TakingOff: return "TakingOff";
    case HoverMissionState::Hovering: return "Hovering";
    case HoverMissionState::WaitingCommand: return "WaitingCommand";
    case HoverMissionState::Failed: return "Failed";
    }
    return "Unknown";
}

//==================================================
// 构造
//==================================================

FlightControl::FlightControl(
    CommandDispatcher& dispatcher,
    DataManager& manager)
    :
    dispatcher(dispatcher),
    manager(manager)
{
}

FlightControl::~FlightControl() noexcept
{
    hoverRunning = false;

    if (hoverThread.joinable())
        hoverThread.join();
}

//==================================================
// 启动任务
//==================================================

void FlightControl::StartHoverMission(const float targetAltitudeMeters)
{
    if (targetAltitudeMeters <= 0.0f)
    {
        SetHoverMissionState(HoverMissionState::Failed);
        ConsoleUI::AddLog("Hover Mission rejected: invalid altitude");
        return;
    }

    hoverTargetAltitudeMeters = targetAltitudeMeters;
    if (SafetyConfig::SAFE_TEST_MODE &&
        !SafetyConfig::REAL_COMMAND_ENABLED)
    {
        hoverRunning = true;
        SetHoverMissionState(HoverMissionState::Hovering);
        ConsoleUI::AddLog(
            "Hover Mission simulated altitude=" +
            std::to_string(targetAltitudeMeters) +
            "m (SAFE_TEST_MODE)");
        SetHoverMissionState(HoverMissionState::WaitingCommand);
        return;
    }

    if (hoverRunning)
    {
        ConsoleUI::AddLog("Hover Mission Running");
        return;
    }

    if (hoverThread.joinable())
    {
        hoverThread.join();
    }

    if (!manager.IsOnline())
    {
        ConsoleUI::AddLog("Vehicle Offline");
        return;
    }

    hoverRunning = true;
    SetHoverMissionState(HoverMissionState::Guided);

    hoverThread =
        std::thread(
            &FlightControl::HoverMission,
            this);
}

//==================================================
// 停止任务
//==================================================

void FlightControl::StopHoverMission()
{
    hoverRunning = false;
    SetHoverMissionState(HoverMissionState::Idle);
    ConsoleUI::AddLog("Hover Mission stopped by explicit command");
}

//==================================================
// 自动悬停
//==================================================

void FlightControl::HoverMission()
{
    ConsoleUI::AddLog("===== Hover Mission Start =====");

    const FlightData target = manager.GetFlightData();

    if (!target.online ||
        target.systemID == 0 ||
        target.componentID == 0)
    {
        ConsoleUI::AddLog("Mission rejected: invalid vehicle target");
        hoverRunning = false;
        SetHoverMissionState(HoverMissionState::Failed);
        return;
    }

    //---------------------------------
    // GUIDED
    //---------------------------------

    dispatcher.QueueGuided(
        target.systemID,
        target.componentID);

    if (!WaitMode(4, 5.0f))
    {
        ConsoleUI::AddLog("GUIDED confirmation timeout");
        hoverRunning = false;
        SetHoverMissionState(HoverMissionState::Failed);
        return;
    }

    //---------------------------------
    // ARM
    //---------------------------------

    SetHoverMissionState(HoverMissionState::Arming);
    dispatcher.QueueArm(
        target.systemID,
        target.componentID);

    if (!WaitArmed(true, 8.0f))
    {
        ConsoleUI::AddLog("ARM rejected or confirmation timeout");
        hoverRunning = false;
        SetHoverMissionState(HoverMissionState::Failed);
        return;
    }

    //---------------------------------
    // TAKEOFF
    //---------------------------------

    SetHoverMissionState(HoverMissionState::TakingOff);
    const float targetAltitude = hoverTargetAltitudeMeters.load();
    Takeoff(targetAltitude);

    ConsoleUI::AddLog(
        "Takeoff target=" + std::to_string(targetAltitude) + "m");

    //---------------------------------
    // 等待高度
    //---------------------------------

    if (!WaitAltitude(
        (std::max)(0.1f, targetAltitude - 0.2f),
        20))
    {
        ConsoleUI::AddLog("Takeoff Failed");

        hoverRunning = false;
        SetHoverMissionState(HoverMissionState::Failed);
        ConsoleUI::AddLog(
            "No automatic LAND/RTL; waiting for operator decision");
        return;
    }

    //---------------------------------
    // 悬停
    //---------------------------------

    SetHoverMissionState(HoverMissionState::Hovering);
    ConsoleUI::AddLog("Hovering at target altitude");
    SetHoverMissionState(HoverMissionState::WaitingCommand);
    ConsoleUI::AddLog(
        "WaitingCommand: operator must explicitly select LAND or RTL");
}

//==================================================
// 起飞
//==================================================

void FlightControl::Takeoff(float altitude)
{
    const FlightData target = manager.GetFlightData();

    if (!target.online)
    {
        ConsoleUI::AddLog("Takeoff blocked: vehicle offline");
        return;
    }

    dispatcher.QueueTakeoff(
        target.systemID,
        target.componentID,
        altitude);
}

//==================================================
// LAND
//==================================================

void FlightControl::Land()
{
    const FlightData target = manager.GetFlightData();

    if (!target.online)
    {
        ConsoleUI::AddLog("Land not sent: vehicle offline");
        return;
    }

    dispatcher.QueueLand(
        target.systemID,
        target.componentID);
}

//==================================================
// RTL
//==================================================

void FlightControl::RTL()
{
    const FlightData target = manager.GetFlightData();

    if (!target.online)
    {
        ConsoleUI::AddLog("RTL not sent: vehicle offline");
        return;
    }

    dispatcher.QueueRTL(
        target.systemID,
        target.componentID);
}

//==================================================
// Hover
//==================================================

void FlightControl::Hover()
{
}

HoverMissionState FlightControl::GetHoverMissionState() const
{
    return hoverState.load();
}

float FlightControl::GetHoverTargetAltitudeMeters() const
{
    return hoverTargetAltitudeMeters.load();
}

void FlightControl::SetHoverMissionState(const HoverMissionState state)
{
    hoverState = state;
    ConsoleUI::AddLog(
        std::string("HoverState=") + HoverMissionStateName(state));
}

//==================================================
// 等待高度
//==================================================

bool FlightControl::WaitAltitude(
    float target,
    float timeout)
{
    auto start =
        chrono::steady_clock::now();

    while (hoverRunning)
    {
        const FlightData data = manager.GetFlightData();

        if (!data.online || !data.altitudeValid)
        {
            ConsoleUI::AddLog("Altitude unavailable");
            return false;
        }

        const float altitude = data.relativeAltitude;

        ConsoleUI::AddLog(
            "Relative Altitude: "
            + std::to_string(altitude)
            + " m");

        if (altitude >= target)
        {
            ConsoleUI::AddLog("Altitude Reached");
            return true;
        }

        auto now =
            chrono::steady_clock::now();

        double t =
            chrono::duration<double>(
                now - start).count();

        if (t > timeout)
        {
            ConsoleUI::AddLog("Altitude Timeout");
            return false;
        }

        this_thread::sleep_for(
            chrono::milliseconds(200));
    }

    return false;
}

bool FlightControl::WaitMode(
    int targetMode,
    float timeout)
{
    const auto start = chrono::steady_clock::now();

    while (hoverRunning)
    {
        const FlightData data = manager.GetFlightData();

        if (!data.online)
        {
            return false;
        }

        if (data.mode == targetMode)
        {
            return true;
        }

        if (chrono::duration<double>(
            chrono::steady_clock::now() - start).count() > timeout)
        {
            return false;
        }

        this_thread::sleep_for(chrono::milliseconds(100));
    }

    return false;
}

bool FlightControl::WaitArmed(
    bool targetArmed,
    float timeout)
{
    const auto start = chrono::steady_clock::now();

    while (hoverRunning)
    {
        const FlightData data = manager.GetFlightData();

        if (!data.online)
        {
            return false;
        }

        if (data.armed == targetArmed)
        {
            return true;
        }

        if (chrono::duration<double>(
            chrono::steady_clock::now() - start).count() > timeout)
        {
            return false;
        }

        this_thread::sleep_for(chrono::milliseconds(100));
    }

    return false;
}

