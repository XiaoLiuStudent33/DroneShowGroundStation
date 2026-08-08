/**
 * @brief 抽象从 Console 共享 MAVLink Socket 向指定 Remote Endpoint 发送 Datagram 的能力。
 *
 * 分类：Production。
 * 输入：目标 IP、目标 Source Port 和 MAVLink 字节；输出：UDP 发送结果。
 * 调用链：DroneSession/UdpClient -> IMavlinkDatagramSender -> MavlinkUdpReceiver。
 */
#pragma once

#include <cstdint>
#include <string>

// Sending boundary used by discovered DroneSessions. The concrete endpoint
// owns the bound UDP socket; a Session only supplies its discovered target.
class IMavlinkDatagramSender
{
public:
    virtual ~IMavlinkDatagramSender() = default;

    virtual bool SendTo(
        const std::string& targetIp,
        std::uint16_t targetPort,
        const std::uint8_t* data,
        int length) = 0;
};
