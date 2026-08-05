#include "MavlinkReadOnlyIngress.h"

MavlinkReadOnlyIngress::MavlinkReadOnlyIngress(
    DroneManager& droneManagerValue,
    MavlinkDiscoveryService& discoveryServiceValue)
    : droneManager(droneManagerValue),
      discoveryService(discoveryServiceValue)
{
}

MavlinkIngressStats MavlinkReadOnlyIngress::SubmitDatagram(
    const std::uint8_t* bytes,
    const std::size_t byteCount,
    const std::string& sourceIp,
    const std::uint16_t sourcePort)
{
    MavlinkIngressStats stats;
    if (bytes == nullptr || byteCount == 0 || sourceIp.empty() || sourcePort == 0)
    {
        stats.ignoredMessages = 1;
        return stats;
    }

    EndpointParserState& parserState =
        parserStates[MakeEndpointKey(sourceIp, sourcePort)];

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

        ++stats.decodedMessages;
        if (decodedMessage.msgid == MAVLINK_MSG_ID_HEARTBEAT)
        {
            const MavlinkDiscoveryResult result =
                discoveryService.IngestMessage(
                    decodedMessage,
                    sourceIp,
                    sourcePort);
            if (result == MavlinkDiscoveryResult::Registered ||
                result == MavlinkDiscoveryResult::Refreshed)
            {
                ++stats.heartbeatsApplied;
            }
            else if (result ==
                MavlinkDiscoveryResult::DroneIdentityConflict)
            {
                ++stats.identityConflicts;
            }
            else
            {
                ++stats.ignoredMessages;
            }
            continue;
        }

        DiscoveredDroneIdentity identity;
        identity.systemId = decodedMessage.sysid;
        identity.componentId = decodedMessage.compid;
        identity.sourceIp = sourceIp;
        identity.sourcePort = sourcePort;
        identity.observedAt = std::chrono::steady_clock::now();

        DroneSession* session = droneManager.FindDroneByIdentity(identity);
        if (session != nullptr &&
            decodedMessage.msgid == MAVLINK_MSG_ID_COMMAND_ACK &&
            session->ApplyDiscoveredCommandMessage(decodedMessage, identity))
        {
            ++stats.commandMessagesApplied;
        }
        else if (session != nullptr &&
            session->ApplyDiscoveredTelemetry(decodedMessage, identity))
        {
            ++stats.telemetryApplied;
        }
        else
        {
            ++stats.ignoredMessages;
        }
    }

    return stats;
}

std::string MavlinkReadOnlyIngress::MakeEndpointKey(
    const std::string& sourceIp,
    const std::uint16_t sourcePort)
{
    return sourceIp + ":" + std::to_string(sourcePort);
}
