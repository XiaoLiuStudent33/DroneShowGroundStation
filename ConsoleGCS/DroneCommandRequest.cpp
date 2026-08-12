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
    }

    return "UNKNOWN";
}
