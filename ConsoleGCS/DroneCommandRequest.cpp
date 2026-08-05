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
    }

    return "UNKNOWN";
}
