/**
 * @brief 从 MAVLink HEARTBEAT 生成可注册的 Remote Endpoint 身份。
 *
 * 分类：Production。
 * 输入：Ingress 已解码消息或测试 Datagram；输出：DroneManager 注册/刷新结果。
 * 调用链：MavlinkReadOnlyIngress -> MavlinkDiscoveryService -> DroneManager。
 * 关键约束：SYSID/COMPID 仅是 MAVLink 路由元数据，不能替代 Endpoint Fleet 身份。
 */
#pragma once

#include "DroneDiscovery.h"
#include "DroneManager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

extern "C"
{
#include "ardupilotmega/mavlink.h"
}

enum class MavlinkDiscoveryResult
{
    Ignored,
    Registered,
    Refreshed,
    DroneIdentityConflict,
    Rejected
};

// Phase 5.1A discovery/registration boundary.
//
// It accepts already-received MAVLink bytes plus their source endpoint. The
// remote IP:port is the discovered identity; SYSID/ComponentID are retained
// only as MAVLink routing fields. It deliberately owns no socket and sends no
// MAVLink messages.
class MavlinkDiscoveryService
{
public:
    MavlinkDiscoveryService(
        DroneManager& droneManager,
        DroneSessionConfig sessionDefaults);

    MavlinkDiscoveryResult IngestDatagram(
        const std::uint8_t* bytes,
        std::size_t byteCount,
        const std::string& sourceIp,
        std::uint16_t sourcePort);

    MavlinkDiscoveryResult IngestMessage(
        const mavlink_message_t& message,
        const std::string& sourceIp,
        std::uint16_t sourcePort);

    void Update(std::chrono::steady_clock::time_point now);

private:
    struct EndpointParserState
    {
        mavlink_message_t messageBuffer{};
        mavlink_status_t parseStatus{};
    };

    DroneManager& droneManager;
    DroneSessionConfig sessionDefaults;
    std::unordered_map<std::string, EndpointParserState> parserStates;
};
