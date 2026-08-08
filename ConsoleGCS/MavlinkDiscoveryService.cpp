/**
 * @brief 实现 HEARTBEAT 过滤、身份提取和 Session 注册刷新。
 *
 * 分类：Production。
 * 输入：非 GCS HEARTBEAT 及其 Remote IP/Source Port；输出：稳定 DroneID 对应的 DroneSession。
 * 相同 Endpoint 恢复时必须刷新原 Session，而不是创建重复 Fleet 成员。
 */
#include "MavlinkDiscoveryService.h"

#include <utility>

MavlinkDiscoveryService::MavlinkDiscoveryService(
    DroneManager& droneManagerValue,
    DroneSessionConfig sessionDefaultsValue)
    : droneManager(droneManagerValue),
      sessionDefaults(std::move(sessionDefaultsValue))
{
}

MavlinkDiscoveryResult MavlinkDiscoveryService::IngestMessage(
    const mavlink_message_t& message,
    const std::string& sourceIp,
    const std::uint16_t sourcePort)
{
    if (message.msgid != MAVLINK_MSG_ID_HEARTBEAT ||
        message.sysid == 0 ||
        message.compid == 0 ||
        sourceIp.empty() ||
        sourcePort == 0)
    {
        return MavlinkDiscoveryResult::Ignored;
    }

    mavlink_heartbeat_t heartbeat{};
    mavlink_msg_heartbeat_decode(&message, &heartbeat);
    if (heartbeat.type == MAV_TYPE_GCS)
    {
        return MavlinkDiscoveryResult::Ignored;
    }

    DiscoveredDroneIdentity identity;
    identity.systemId = message.sysid;
    identity.componentId = message.compid;
    identity.sourceIp = sourceIp;
    identity.sourcePort = sourcePort;
    identity.observedAt = std::chrono::steady_clock::now();

    const DroneRegistrationResult registration =
        droneManager.RegisterOrRefreshDrone(identity, sessionDefaults);
    if (registration.status ==
        DroneRegistrationStatus::DroneIdentityConflict)
    {
        return MavlinkDiscoveryResult::DroneIdentityConflict;
    }

    if (registration.status == DroneRegistrationStatus::Rejected ||
        registration.session == nullptr)
    {
        return MavlinkDiscoveryResult::Rejected;
    }

    if (!registration.session->ApplyDiscoveredHeartbeat(
        message,
        identity))
    {
        return MavlinkDiscoveryResult::Rejected;
    }

    return registration.status == DroneRegistrationStatus::Created
        ? MavlinkDiscoveryResult::Registered
        : MavlinkDiscoveryResult::Refreshed;
}

void MavlinkDiscoveryService::Update(
    const std::chrono::steady_clock::time_point now)
{
    droneManager.RefreshHeartbeatStates(now);
}
