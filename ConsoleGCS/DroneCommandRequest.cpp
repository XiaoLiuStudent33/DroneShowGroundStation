/**
 * @brief 提供领域命令类型的稳定文本名称。
 *
 * 分类：Production；供日志和 DSP 命令结果使用，不负责 MAVLink 命令编码。
 */
#include "DroneCommandRequest.h"

const char* DroneCommandTypeName(DroneCommandType type)
{
    switch (type)
    {
    case DroneCommandType::Arm:
        return "ARM";
    case DroneCommandType::Disarm:
        return "DISARM";
    case DroneCommandType::Takeoff:
        return "TAKEOFF";
    case DroneCommandType::Land:
        return "LAND";
    case DroneCommandType::RTL:
        return "RTL";
    case DroneCommandType::SetMode:
        return "SET_MODE";
    case DroneCommandType::HoverTest:
        return "HOVER_TEST";
    case DroneCommandType::SetLedColor:
        return "SET_LED_COLOR";
    case DroneCommandType::TimeStart:
        return "TIME_START";
    case DroneCommandType::TimeStop:
        return "TIME_STOP";
    case DroneCommandType::ExecuteDanceStep:
        return "EXECUTE_DANCE_STEP";
    case DroneCommandType::EndDanceStep:
        return "END_DANCE_STEP";
    case DroneCommandType::MissionPause:
        return "MISSION_PAUSE";
    case DroneCommandType::MissionForward:
        return "MISSION_FORWARD";
    case DroneCommandType::MissionBackward:
        return "MISSION_BACKWARD";
    case DroneCommandType::ChangeSpeed:
        return "CHANGE_SPEED";
    case DroneCommandType::TriggerSmoke:
        return "TRIGGER_SMOKE";
    case DroneCommandType::StopTelemetry:
        return "STOP_TELEMETRY";
    case DroneCommandType::ResumeTelemetry:
        return "RESUME_TELEMETRY";
    case DroneCommandType::DisableLedOutput:
        return "DISABLE_LED_OUTPUT";
    case DroneCommandType::RestoreLedOutput:
        return "RESTORE_LED_OUTPUT";
    case DroneCommandType::Wake:
        return "WAKE";
    case DroneCommandType::Standby:
        return "STANDBY";
    case DroneCommandType::Reboot:
        return "REBOOT";
    case DroneCommandType::Shutdown:
        return "SHUTDOWN";
    }

    return "UNKNOWN";
}
