#include "MavlinkDiscoveryService.h"

#include <utility>

MavlinkDiscoveryService::MavlinkDiscoveryService(
    DroneManager& droneManagerValue,
    DroneSessionConfig sessionDefaultsValue)
    : droneManager(droneManagerValue),
      sessionDefaults(std::move(sessionDefaultsValue))
{
}

MavlinkDiscoveryResult MavlinkDiscoveryService::IngestDatagram(
    const std::uint8_t* bytes,
    const std::size_t byteCount,
    const std::string& sourceIp,
    const std::uint16_t sourcePort)
{
    if (bytes == nullptr || byteCount == 0 || sourceIp.empty() || sourcePort == 0)
    {
        return MavlinkDiscoveryResult::Rejected;
    }

    EndpointParserState& parserState =
        parserStates[MakeEndpointKey(sourceIp, sourcePort)];
    MavlinkDiscoveryResult finalResult = MavlinkDiscoveryResult::Ignored;

    for (std::size_t index = 0; index < byteCount; ++index)
    {
        mavlink_message_t decodedMessage{};
        mavlink_status_t decodedStatus{};
        if (mavlink_frame_char_buffer(
            &parserState.messageBuffer,
            &parserState.parseStatus,
            bytes[index],
            &decodedMessage,
            &decodedStatus) != MAVLINK_FRAMING_OK)
        {
            continue;
        }

        const MavlinkDiscoveryResult result =
            IngestMessage(decodedMessage, sourceIp, sourcePort);
        if (result != MavlinkDiscoveryResult::Ignored)
        {
            finalResult = result;
        }
    }

    return finalResult;
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

std::string MavlinkDiscoveryService::MakeEndpointKey(
    const std::string& sourceIp,
    const std::uint16_t sourcePort)
{
    return sourceIp + ":" + std::to_string(sourcePort);
}
