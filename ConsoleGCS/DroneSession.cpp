/**
 * @brief 实现单个 Endpoint Session 的 Online/Offline、遥测应用和命令调度。
 *
 * 分类：Production / Compatibility。
 * 调用链：DroneManager/Ingress -> DroneSession -> DataManager/CommandDispatcher/MavlinkCommand。
 * 相同 Endpoint 的新 HEARTBEAT 必须恢复原 Session；旧独立 Socket 接收路径仅用于兼容模式。
 */
#include "DroneSession.h"

#include "ConsoleUI.h"
#include "SafetyConfig.h"

#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include "ardupilotmega/mavlink.h"
}

namespace
{
    std::string SessionPrefix(const std::string& droneId)
    {
        return "[" + droneId + "] ";
    }
}

const char* DroneSessionStateName(DroneSessionState state)
{
    switch (state)
    {
    case DroneSessionState::Stopped:
        return "STOPPED";
    case DroneSessionState::Initializing:
        return "INITIALIZING";
    case DroneSessionState::WaitingForHeartbeat:
        return "WAITING_HEARTBEAT";
    case DroneSessionState::Online:
        return "ONLINE";
    case DroneSessionState::HeartbeatTimeout:
        return "HEARTBEAT_TIMEOUT";
    case DroneSessionState::Error:
        return "ERROR";
    }

    return "UNKNOWN";
}

DroneSession::DroneSession(DroneSessionConfig configValue)
    : config(std::move(configValue)),
      dispatcher(udp),
      flightControl(dispatcher, dataManager)
{
}

DroneSession::~DroneSession() noexcept
{
    Stop();
}

bool DroneSession::Start()
{
    if (started)
    {
        return true;
    }

    SetState(DroneSessionState::Initializing);

    if (!config.receiveOnly &&
        config.realCommandEnabled &&
        !(config.sharedMavlinkEndpoint != nullptr
            ? udp.AttachSharedEndpoint(
                *config.sharedMavlinkEndpoint,
                config.targetIp,
                config.targetPort)
            : udp.Init(
                config.targetIp,
                config.targetPort,
                config.localIp,
                config.localPort)))
    {
        SetState(DroneSessionState::Error);
        return false;
    }

    workingData = FlightData{};
    dataManager.Update(workingData);
    requestSent = false;
    lastGcsHeartbeat = std::chrono::steady_clock::now();
    started = true;
    SetState(DroneSessionState::WaitingForHeartbeat);

    ConsoleUI::AddLog(
        SessionPrefix(config.droneId) +
        (config.receiveOnly
            ? "receive-only MAVLink session started"
            : !config.realCommandEnabled
            ? "MAVLink command transport disabled"
            : std::string(config.sharedMavlinkEndpoint != nullptr
                ? "shared MAVLink endpoint "
                : "MAVLink session started ") +
                config.localIp + ":" + std::to_string(config.localPort) +
                " -> " + config.targetIp + ":" +
                std::to_string(config.targetPort)));

    return true;
}

void DroneSession::Stop()
{
    if (!started && state == DroneSessionState::Stopped)
    {
        return;
    }

    flightControl.StopHoverMission();
    udp.Close();
    started = false;
    requestSent = false;
    SetState(DroneSessionState::Stopped);
}

void DroneSession::Tick(
    std::chrono::steady_clock::time_point now)
{
    if (!started)
    {
        return;
    }

    if (!config.receiveOnly &&
        config.realCommandEnabled &&
        now - lastGcsHeartbeat >= std::chrono::seconds(1))
    {
        SendHeartbeat();
        lastGcsHeartbeat = now;
    }

    RefreshHeartbeatStatus(now);

    ProcessIncomingData(now);
    ProcessDispatchResults(now);

    const FlightData currentData = dataManager.GetFlightData();

    if (!config.receiveOnly && currentData.online && !requestSent)
    {
        RequestDataStream(
            currentData.systemID,
            currentData.componentID);
        requestSent = true;

        ConsoleUI::AddLog(
            SessionPrefix(config.droneId) +
            "Vehicle Connected SYSID=" +
            std::to_string(currentData.systemID));
    }
}

void DroneSession::RefreshHeartbeatStatus(
    const std::chrono::steady_clock::time_point now)
{
    if (!started)
    {
        return;
    }

    if (dataManager.RefreshOnlineStatus(
        now,
        std::chrono::milliseconds(config.heartbeatTimeoutMs)))
    {
        const FlightData connection = dataManager.GetFlightData();

        if (connection.online)
        {
            SetState(DroneSessionState::Online);
        }
        else
        {
            // Offline 只是在线状态变化：Session、DroneID、SessionOrder 和最后遥测
            // 都继续保留。requestSent 重置后，原 Endpoint 恢复时可重新请求数据流。
            SetState(DroneSessionState::HeartbeatTimeout);
            requestSent = false;
        }
    }
}

DroneSessionCommandSubmission DroneSession::SubmitCommand(
    const DroneCommandRequest& request)
{
    DroneSessionCommandSubmission submission;
    const FlightData commandTarget = dataManager.GetFlightData();
    const std::string requestPrefix =
        SessionPrefix(config.droneId) +
        "Command=" + std::to_string(request.commandId) + " ";

    if (config.receiveOnly)
    {
        submission.error = "receive-only session rejects control commands";
        return submission;
    }

    if (request.commandType == DroneCommandType::HoverTest &&
        !request.parameters.startHoverTest)
    {
        flightControl.StopHoverMission();
        ConsoleUI::AddLog(requestPrefix + "Hover Mission Stop");
        submission.accepted = true;
        submission.completedImmediately = true;
        return submission;
    }

    if (!config.realCommandEnabled && SafetyConfig::SAFE_TEST_MODE)
    {
        if (request.commandType == DroneCommandType::HoverTest)
        {
            if (request.parameters.hoverAltitudeMeters <= 0.0f)
            {
                submission.error = "invalid hover altitude";
                return submission;
            }

            flightControl.StartHoverMission(
                request.parameters.hoverAltitudeMeters);
            submission.detail = "Hovering->WaitingCommand";
        }
        else if (request.commandType == DroneCommandType::Land ||
            request.commandType == DroneCommandType::RTL)
        {
            // HoverTest never generates LAND/RTL. Only an explicit operator
            // command clears the waiting state.
            flightControl.StopHoverMission();
            submission.detail = "Hover stopped by explicit operator command";
        }

        ConsoleUI::AddLog(
            requestPrefix + DroneCommandTypeName(request.commandType) +
            " simulated (SAFE_TEST_MODE)");
        submission.accepted = true;
        submission.completedImmediately = true;
        submission.simulated = true;
        return submission;
    }

    if (!config.realCommandEnabled)
    {
        submission.error = "real MAVLink commands are disabled";
        return submission;
    }

    if (!commandTarget.online)
    {
        ConsoleUI::AddLog(
            requestPrefix +
            "Command blocked: vehicle offline");
        submission.error = "vehicle offline";
        return submission;
    }

    switch (request.commandType)
    {
    case DroneCommandType::Arm:
        ConsoleUI::AddLog(requestPrefix + "ARM Command");
        submission.dispatchRequestIds.push_back(dispatcher.QueueArm(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::Disarm:
        if (commandTarget.armed &&
            (!commandTarget.altitudeValid ||
             commandTarget.relativeAltitude > 0.3f))
        {
            ConsoleUI::AddLog(
                requestPrefix +
                "DISARM blocked: landing not confirmed");
            submission.error = "landing not confirmed";
            return submission;
        }

        ConsoleUI::AddLog(requestPrefix + "DISARM Command");
        submission.dispatchRequestIds.push_back(dispatcher.QueueDisarm(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::SetMode:
        if (!ArduCopterCustomMode::IsSupported(request.parameters.customMode))
        {
            submission.error = "unsupported ArduPilot Copter flight mode";
            return submission;
        }

        ConsoleUI::AddLog(
            requestPrefix + "SET_MODE " +
            ArduCopterCustomMode::Name(request.parameters.customMode) +
            " target_system=" + std::to_string(commandTarget.systemID) +
            " base_mode=" + std::to_string(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) +
            " custom_mode=" + std::to_string(request.parameters.customMode));
        submission.dispatchRequestIds.push_back(dispatcher.QueueSetMode(
            commandTarget.systemID,
            commandTarget.componentID,
            static_cast<std::uint8_t>(request.parameters.customMode)));
        submission.accepted = true;
        return submission;

    case DroneCommandType::Takeoff:
        if (request.parameters.takeoffAltitudeMeters <= 0.0f ||
            request.parameters.takeoffAltitudeMeters >
                SafetyConfig::TAKEOFF_ALTITUDE_LIMIT_METERS)
        {
            ConsoleUI::AddLog(
                requestPrefix + "TAKEOFF blocked: altitude must be within (0, " +
                std::to_string(SafetyConfig::TAKEOFF_ALTITUDE_LIMIT_METERS) + "]m");
            submission.error = "takeoff altitude exceeds configured limit";
            return submission;
        }

        ConsoleUI::AddLog(
            requestPrefix + "TAKEOFF Command altitude=" +
            std::to_string(request.parameters.takeoffAltitudeMeters));
        submission.dispatchRequestIds.push_back(dispatcher.QueueTakeoff(
            commandTarget.systemID,
            commandTarget.componentID,
            request.parameters.takeoffAltitudeMeters));
        submission.accepted = true;
        return submission;

    case DroneCommandType::RTL:
        flightControl.StopHoverMission();
        ConsoleUI::AddLog(requestPrefix + "RTL Command");
        submission.dispatchRequestIds.push_back(dispatcher.QueueRTL(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::Land:
        flightControl.StopHoverMission();
        ConsoleUI::AddLog(requestPrefix + "LAND Command");
        submission.dispatchRequestIds.push_back(dispatcher.QueueLand(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::HoverTest:
        if (request.parameters.hoverAltitudeMeters <= 0.0f ||
            request.parameters.hoverAltitudeMeters >
                SafetyConfig::HOVER_ALTITUDE_LIMIT_METERS)
        {
            ConsoleUI::AddLog(
                requestPrefix + "HOVER_TEST blocked: altitude must be within (0, " +
                std::to_string(SafetyConfig::HOVER_ALTITUDE_LIMIT_METERS) + "]m");
            submission.error = "hover altitude exceeds configured limit";
            return submission;
        }

        ConsoleUI::AddLog(requestPrefix + "Hover Mission Start");
        flightControl.StartHoverMission(
            request.parameters.hoverAltitudeMeters);
        submission.accepted = true;
        submission.completedImmediately = true;
        submission.detail = "Hovering->WaitingCommand";
        return submission;
    }

    submission.error = "unsupported command";
    return submission;
}

std::vector<CommandDispatchResult> DroneSession::ConsumeCommandResults()
{
    std::vector<CommandDispatchResult> results;
    results.reserve(completedCommandResults.size());

    while (!completedCommandResults.empty())
    {
        results.push_back(completedCommandResults.front());
        completedCommandResults.pop_front();
    }

    return results;
}

bool DroneSession::RefreshDiscoveredPresence(
    const DiscoveredDroneIdentity& identity)
{
    if (!started ||
        identity.sourceIp != config.targetIp ||
        identity.sourcePort != config.targetPort)
    {
        return false;
    }

    config.discoveredSystemId = identity.systemId;
    config.discoveredComponentId = identity.componentId;
    config.targetIp = identity.sourceIp;
    config.targetPort = identity.sourcePort;

    // 相同 Endpoint 的 HEARTBEAT 恢复原 Session；SYSID/COMPID 可以刷新为
    // 当前协议路由元数据，但不会生成新的 Fleet 身份。
    workingData.systemID = identity.systemId;
    workingData.componentID = identity.componentId;
    workingData.online = true;
    workingData.lastHeartbeat =
        identity.observedAt == std::chrono::steady_clock::time_point{}
            ? std::chrono::steady_clock::now()
            : identity.observedAt;
    dataManager.Update(workingData);
    SetState(DroneSessionState::Online);
    return true;
}

bool DroneSession::ApplyDiscoveredHeartbeat(
    const mavlink_message_t& message,
    const DiscoveredDroneIdentity& identity)
{
    if (message.msgid != MAVLINK_MSG_ID_HEARTBEAT ||
        message.sysid != config.discoveredSystemId ||
        message.compid != config.discoveredComponentId ||
        identity.systemId != config.discoveredSystemId ||
        identity.componentId != config.discoveredComponentId ||
        identity.sourceIp != config.targetIp ||
        identity.sourcePort != config.targetPort)
    {
        return false;
    }

    mavlink_heartbeat_t heartbeat{};
    mavlink_msg_heartbeat_decode(&message, &heartbeat);
    if (heartbeat.type == MAV_TYPE_GCS)
    {
        return false;
    }

    const int previousMode = workingData.mode;
    const std::string previousModeName = workingData.modeName;
    parser.Parse(message, workingData);
    dataManager.Update(workingData);
    SetState(DroneSessionState::Online);
    if (previousMode != workingData.mode || previousModeName != workingData.modeName)
    {
        ConsoleUI::AddLog(
            "[HEARTBEAT] " + config.droneId +
            " Mode=" + workingData.modeName +
            " custom_mode=" + std::to_string(workingData.mode) +
            " endpoint=" + config.targetIp + ":" +
            std::to_string(config.targetPort));
    }
    return true;
}

bool DroneSession::ApplyDiscoveredTelemetry(
    const mavlink_message_t& message,
    const DiscoveredDroneIdentity& identity)
{
    if (message.sysid != config.discoveredSystemId ||
        message.compid != config.discoveredComponentId ||
        identity.systemId != config.discoveredSystemId ||
        identity.componentId != config.discoveredComponentId ||
        identity.sourceIp != config.targetIp ||
        identity.sourcePort != config.targetPort)
    {
        return false;
    }

    switch (message.msgid)
    {
    case MAVLINK_MSG_ID_ATTITUDE:
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
    case MAVLINK_MSG_ID_GPS_RAW_INT:
    case MAVLINK_MSG_ID_SYS_STATUS:
        parser.Parse(message, workingData);
        dataManager.Update(workingData);
        return true;
    default:
        return false;
    }
}

bool DroneSession::ApplyDiscoveredCommandMessage(
    const mavlink_message_t& message,
    const DiscoveredDroneIdentity& identity)
{
    if (message.msgid != MAVLINK_MSG_ID_COMMAND_ACK ||
        message.sysid != config.discoveredSystemId ||
        identity.systemId != config.discoveredSystemId ||
        identity.sourceIp != config.targetIp ||
        identity.sourcePort != config.targetPort)
    {
        return false;
    }

    parser.Parse(message, workingData);
    dataManager.Update(workingData);
    dispatcher.HandleAck(message);
    return true;
}

const std::string& DroneSession::GetDroneId() const
{
    return config.droneId;
}

std::uint8_t DroneSession::GetSysId() const
{
    return dataManager.GetFlightData().systemID;
}

DroneSessionState DroneSession::GetState() const
{
    return state;
}

const DroneSessionConfig& DroneSession::GetConfig() const
{
    return config;
}

FlightData DroneSession::GetFlightData() const
{
    return dataManager.GetFlightData();
}

DroneAckState DroneSession::GetAckState() const
{
    const FlightData data = dataManager.GetFlightData();
    DroneAckState ack;
    ack.valid = data.commandAckValid;
    ack.command = data.lastAckCommand;
    ack.result = data.lastAckResult;
    ack.receivedAt = data.lastAckTime;
    return ack;
}

const HoverTest& DroneSession::GetHoverTest() const
{
    return hoverTest;
}

HoverMissionState DroneSession::GetHoverMissionState() const
{
    return flightControl.GetHoverMissionState();
}

float DroneSession::GetHoverTargetAltitudeMeters() const
{
    return flightControl.GetHoverTargetAltitudeMeters();
}

void DroneSession::SetState(DroneSessionState newState)
{
    if (state == newState)
    {
        return;
    }

    const DroneSessionState previousState = state;
    state = newState;

    if (state == DroneSessionState::Online)
    {
        ConsoleUI::AddLog(config.droneId + " ONLINE");
    }
    else if (previousState == DroneSessionState::Online)
    {
        ConsoleUI::AddLog(config.droneId + " OFFLINE");
    }
    else if (state == DroneSessionState::Error)
    {
        ConsoleUI::AddLog(config.droneId + " ERROR");
    }
}

void DroneSession::SendHeartbeat()
{
    // MAVLink #0 HEARTBEAT - GCS heartbeat with SYSID=255.
    mavlink_message_t message;
    std::uint8_t buffer[MAVLINK_MAX_PACKET_LEN];

    mavlink_msg_heartbeat_pack(
        255,
        MAV_COMP_ID_MISSIONPLANNER,
        &message,
        MAV_TYPE_GCS,
        MAV_AUTOPILOT_INVALID,
        0,
        0,
        MAV_STATE_ACTIVE);

    const std::uint16_t length =
        mavlink_msg_to_send_buffer(buffer, &message);
    (void)udp.Send(buffer, length);
}

void DroneSession::RequestDataStream(
    std::uint8_t system,
    std::uint8_t component)
{
    // MAVLink #66 REQUEST_DATA_STREAM - requests all streams at 10 Hz.
    mavlink_message_t message;
    std::uint8_t buffer[MAVLINK_MAX_PACKET_LEN];

    mavlink_msg_request_data_stream_pack(
        255,
        MAV_COMP_ID_MISSIONPLANNER,
        &message,
        system,
        component,
        MAV_DATA_STREAM_ALL,
        10,
        1);

    const std::uint16_t length =
        mavlink_msg_to_send_buffer(buffer, &message);
    udp.Send(buffer, length);

    ConsoleUI::AddLog(
        SessionPrefix(config.droneId) + "REQUEST DATA STREAM");
}

void DroneSession::ProcessIncomingData(
    std::chrono::steady_clock::time_point now)
{
    if (!config.realCommandEnabled)
    {
        return;
    }

    std::uint8_t buffer[2048];
    const int length = udp.Receive(buffer, sizeof(buffer));

    if (length <= 0)
    {
        return;
    }

    mavlink_message_t message;
    mavlink_status_t decodedStatus{};

    for (int index = 0; index < length; ++index)
    {
        if (mavlink_frame_char_buffer(
            &parseMessageBuffer,
            &parseStatus,
            buffer[index],
            &message,
            &decodedStatus) != MAVLINK_FRAMING_OK)
        {
            continue;
        }

        if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT)
        {
            mavlink_heartbeat_t heartbeat;
            mavlink_msg_heartbeat_decode(&message, &heartbeat);

            if (heartbeat.type == MAV_TYPE_GCS)
            {
                continue;
            }
        }

        if (workingData.systemID != 0 &&
            message.sysid != workingData.systemID)
        {
            continue;
        }

        const std::uint8_t previousSysId = workingData.systemID;
        parser.Parse(message, workingData);

        if (workingData.lastHeartbeat ==
                std::chrono::steady_clock::time_point{} ||
            now - workingData.lastHeartbeat >
                std::chrono::milliseconds(config.heartbeatTimeoutMs))
        {
            workingData.online = false;
        }

        dataManager.Update(workingData);
        hoverTest.Update(dataManager.GetFlightData());

        if (previousSysId == 0 && workingData.systemID != 0)
        {
            ConsoleUI::AddLog(
                SessionPrefix(config.droneId) +
                "SYSID locked=" + std::to_string(workingData.systemID));
        }

        if (workingData.online)
        {
            SetState(DroneSessionState::Online);
        }

        if (message.msgid == MAVLINK_MSG_ID_COMMAND_ACK)
        {
            dispatcher.HandleAck(message);

            mavlink_command_ack_t ack;
            mavlink_msg_command_ack_decode(&message, &ack);
            ConsoleUI::AddLog(
                SessionPrefix(config.droneId) +
                "ACK cmd=" + std::to_string(ack.command) +
                " result=" + std::to_string(ack.result));
        }

        if (message.msgid == MAVLINK_MSG_ID_STATUSTEXT)
        {
            mavlink_statustext_t statusText;
            mavlink_msg_statustext_decode(&message, &statusText);
            ConsoleUI::AddLog(
                SessionPrefix(config.droneId) +
                "MSG:" + std::string(statusText.text));
        }
    }
}

void DroneSession::ProcessDispatchResults(
    std::chrono::steady_clock::time_point now)
{
    dispatcher.Tick(now);

    const std::vector<CommandDispatchResult> results =
        dispatcher.ConsumeResults();

    for (const CommandDispatchResult& result : results)
    {
        completedCommandResults.push_back(result);
        ConsoleUI::AddLog(
            SessionPrefix(config.droneId) +
            "CMD request=" + std::to_string(result.requestId) +
            " command=" + std::to_string(result.command) +
            " result=" + CommandOutcomeName(result.outcome) +
            " attempts=" + std::to_string(result.attempts));
    }
}
