#pragma once

/**
 * @brief 将项目实际使用的 MAVLink 消息更新到 FlightData。
 *
 * 分类：Production。
 * 输入：已完成帧校验的 mavlink_message_t；输出：当前 Session 的领域状态。
 * 关键约束：消息职责按 ID 分离，尤其 #33 不得覆盖 #24 的绝对 GPS/AMSL。
 */

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