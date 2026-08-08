/**
 * @brief Console 侧共享 MAVLink UDP 端点。
 *
 * 分类：Production。
 * 输入：配置的本地 Bind IP/Port；输出：带 Remote IP/Source Port 的 Datagram，并从同一 Socket 回发 MAVLink。
 * 调用链：ConsoleGCSApplication -> MavlinkUdpReceiver -> MavlinkReadOnlyIngress / DroneSession。
 * 关键约束：所有无人机共享一个绑定 Socket，Remote Endpoint 才是 Session 身份来源。
 */
#pragma once

#include "MavlinkDatagramSender.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>
#include <vector>

struct MavlinkReceivedDatagram
{
    std::vector<std::uint8_t> bytes;
    std::string sourceIp;
    std::uint16_t sourcePort = 0;
};

enum class MavlinkUdpReceiveResult
{
    Received,
    WouldBlock,
    Error
};

// Shared MAVLink endpoint. One bound socket receives all discovered-aircraft
// traffic and sends Session-specific traffic back to each discovered endpoint.
class MavlinkUdpReceiver : public IMavlinkDatagramSender
{
public:
    MavlinkUdpReceiver();
    ~MavlinkUdpReceiver() noexcept;

    MavlinkUdpReceiver(const MavlinkUdpReceiver&) = delete;
    MavlinkUdpReceiver& operator=(const MavlinkUdpReceiver&) = delete;

    bool Start(
        const std::string& bindIp,
        std::uint16_t bindPort,
        std::string& error);
    void Stop();

    MavlinkUdpReceiveResult Receive(
        MavlinkReceivedDatagram& datagram,
        std::string& error);

    bool SendTo(
        const std::string& targetIp,
        std::uint16_t targetPort,
        const std::uint8_t* data,
        int length) override;

    bool IsRunning() const;
    std::uint16_t GetBoundPort() const;

private:
    SOCKET socketHandle = INVALID_SOCKET;
    bool winsockStarted = false;
    std::uint16_t boundPort = 0;
};
