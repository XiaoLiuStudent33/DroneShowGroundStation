/**
 * @brief 实现 Remote Endpoint 隔离的 MAVLink 解码和 Session 路由。
 *
 * 分类：Production。
 * 调用链：MavlinkUdpReceiver -> MavlinkReadOnlyIngress -> Discovery/DroneManager/DroneSession。
 * 只有有效非 GCS HEARTBEAT 可以创建或刷新 Session，其他消息只能进入已知 Endpoint。
 */
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

    DiscoveredDroneIdentity endpointIdentity;
    endpointIdentity.sourceIp = sourceIp;
    endpointIdentity.sourcePort = sourcePort;
    EndpointParserState& parserState =
        parserStates[endpointIdentity.EndpointKey()];

    // The decoder populates these outputs for a completed frame. Reuse them
    // within this datagram instead of clearing a full message for every byte.
    mavlink_message_t decodedMessage{};
    mavlink_status_t decodedStatus{};
    for (std::size_t index = 0; index < byteCount; ++index)
    {
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

        DiscoveredDroneIdentity identity = endpointIdentity;
        identity.systemId = decodedMessage.sysid;
        identity.componentId = decodedMessage.compid;
        identity.observedAt = std::chrono::steady_clock::now();

        DroneSession* session = droneManager.FindDroneByIdentity(identity);
        if (session != nullptr &&
            decodedMessage.msgid == MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL &&
            session->ApplyDiscoveredFtpMessage(decodedMessage, identity))
        {
            ++stats.ftpMessagesApplied;
        }
        else if (session != nullptr &&
            (decodedMessage.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_INT ||
             decodedMessage.msgid == MAVLINK_MSG_ID_MISSION_REQUEST ||
             decodedMessage.msgid == MAVLINK_MSG_ID_MISSION_ACK ||
             decodedMessage.msgid == MAVLINK_MSG_ID_MISSION_COUNT ||
             decodedMessage.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT) &&
            session->ApplyDiscoveredMissionMessage(decodedMessage, identity))
        {
            ++stats.missionMessagesApplied;
        }
        else if (session != nullptr &&
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
