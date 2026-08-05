#pragma once

#include "FlightData.h"

extern "C"
{
#include "ardupilotmega/mavlink.h"
}

class MavlinkParser
{
public:

    void Parse(const mavlink_message_t& msg,
        FlightData& data);
private:

    // 将ArduPilot模式编号转换为字符串
    std::string GetModeName(int mode);

    std::string GetAutopilotName(int type);

    std::string GetVehicleName(int type);

    std::string GetSystemStatusName(int status);
};