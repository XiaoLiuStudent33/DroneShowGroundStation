/**
 * @brief 从 MAVLink HEARTBEAT 生成可注册的 Remote Endpoint 身份。
 *
 * 分类：Production。
 * 输入：Ingress 已解码的 HEARTBEAT；输出：DroneManager 注册/刷新结果。
 * 调用链：MavlinkReadOnlyIngress -> MavlinkDiscoveryService -> DroneManager。
 * 关键约束：SYSID/COMPID 仅是 MAVLink 路由元数据，不能替代 Endpoint Fleet 身份。
 */
#pragma once

#include "DroneDiscovery.h"
#include "DroneManager.h"

#include <cstdint>
#include <string>

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
// It accepts an already-decoded HEARTBEAT plus its source endpoint. The
// remote IP:port is the discovered identity; SYSID/ComponentID are retained
// only as MAVLink routing fields. It deliberately owns no socket and sends no
// MAVLink messages.
class MavlinkDiscoveryService
{
public:
    MavlinkDiscoveryService(
        DroneManager& droneManager,
        DroneSessionConfig sessionDefaults);

    MavlinkDiscoveryResult IngestMessage(
        const mavlink_message_t& message,
        const std::string& sourceIp,
        std::uint16_t sourcePort);

    void Update(std::chrono::steady_clock::time_point now);

private:
    DroneManager& droneManager;
    DroneSessionConfig sessionDefaults;
};
