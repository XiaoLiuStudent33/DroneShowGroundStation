/**
 * @brief 实现单个 Endpoint Session 的 Online/Offline、遥测应用和命令调度。
 *
 * 分类：Production。
 * 调用链：DroneManager/Ingress -> DroneSession -> DataManager/CommandDispatcher/MavlinkCommand。
 * 相同 Endpoint 的新 HEARTBEAT 必须恢复原 Session；接收统一由 MavlinkUdpReceiver/Ingress 路由。
 */
#include "DroneSession.h"

#include "ConsoleUI.h"
#include "SafetyConfig.h"

#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include "mdhk/mavlink.h"
}

namespace
{
    std::string SessionPrefix(const std::string& droneId)
    {
        return "[" + droneId + "] ";
    }

    const char* MavSeverityName(const std::uint8_t severity)
    {
        switch (severity)
        {
        case MAV_SEVERITY_EMERGENCY: return "EMERGENCY";
        case MAV_SEVERITY_ALERT: return "ALERT";
        case MAV_SEVERITY_CRITICAL: return "CRITICAL";
        case MAV_SEVERITY_ERROR: return "ERROR";
        case MAV_SEVERITY_WARNING: return "WARNING";
        case MAV_SEVERITY_NOTICE: return "NOTICE";
        case MAV_SEVERITY_INFO: return "INFO";
        case MAV_SEVERITY_DEBUG: return "DEBUG";
        default: return "UNKNOWN";
        }
    }

    std::string StatusTextString(const mavlink_statustext_t& status)
    {
        std::size_t length = 0;
        while (length < sizeof(status.text) && status.text[length] != '\0')
        {
            ++length;
        }
        return std::string(status.text, status.text + length);
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

    if ((config.realCommandEnabled || config.mavFtpServiceEnabled ||
         config.missionUploadServiceEnabled) &&
        (config.sharedMavlinkEndpoint == nullptr ||
         !udp.AttachSharedEndpoint(
             *config.sharedMavlinkEndpoint,
             config.remoteIp,
             config.remotePort)))
    {
        SetState(DroneSessionState::Error);
        return false;
    }

    workingData = FlightData{};
    dataManager.Update(workingData);
    requestSent = false;
    lastGcsHeartbeat = std::chrono::steady_clock::now();
    started = true;
    if (config.mavFtpServiceEnabled)
    {
        dronegcs::mavftp::PeerIdentity peer;
        peer.droneId = config.droneId;
        peer.remoteIp = config.remoteIp;
        peer.remotePort = static_cast<std::uint16_t>(config.remotePort);
        peer.systemId = config.discoveredSystemId;
        peer.componentId = config.discoveredComponentId;
        dronegcs::mavftp::ClientConfig clientConfig;
        clientConfig.requestTimeout =
            std::chrono::milliseconds(config.mavFtpRequestTimeoutMs);
        clientConfig.terminateTimeout =
            std::chrono::milliseconds(config.mavFtpTerminateTimeoutMs);
        clientConfig.maximumRetries = config.mavFtpRequestRetryCount;
        clientConfig.writeWindow = config.mavFtpWriteWindow;
        clientConfig.writePermitPool = config.mavFtpWritePermitPool;
        mavFtpClient = std::make_unique<dronegcs::mavftp::Client>(
            *this, peer, clientConfig);
    }
    SetState(DroneSessionState::WaitingForHeartbeat);

    ConsoleUI::AddLog(
        SessionPrefix(config.droneId) +
        (config.mavFtpServiceEnabled && !config.realCommandEnabled
            ? "endpoint-bound MAVFTP probe service enabled"
            : config.receiveOnly
            ? "receive-only MAVLink session started"
            : !config.realCommandEnabled
            ? "MAVLink command transport disabled"
            : "shared MAVLink endpoint -> " + config.remoteIp + ":" +
                std::to_string(config.remotePort)));

    return true;
}

void DroneSession::Stop()
{
    if (!started && state == DroneSessionState::Stopped)
    {
        return;
    }

    flightControl.StopHoverMission();
    if (mavFtpClient) mavFtpClient->Shutdown();
    mavFtpClient.reset();
    receivedMissionCount.reset();
    missionVerificationSink = nullptr;
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

    if (mavFtpClient) mavFtpClient->Tick(now);

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
            if (mavFtpClient && mavFtpClient->IsBusy())
                mavFtpClient->Cancel();
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
        // GPS/AMSL validity is not relative-altitude validity or landed state.
        // Nor does altitude relative to home prove ground contact (indoor GPS
        // loss and landing above home must not prevent an ordinary request).
        // The flight controller owns disarm safety checks: send normal #400
        // (param1=0, param2=0), never force or retry with a force override.
        // Queueing is not success; retain the normal matching ACK/timeout path.
        ConsoleUI::AddLog(requestPrefix +
            "DISARM Command (normal; flight-controller safety checks; awaiting ACK)");
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
        ConsoleUI::AddLog(
            requestPrefix +
            "RTL SET_MODE custom_mode=6 awaiting HEARTBEAT confirmation");
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

    case DroneCommandType::MissionPause:
        ConsoleUI::AddLog(
            requestPrefix +
            "MISSION_PAUSE legacy MDHK command=17 param1=1 awaiting ACK");
        submission.dispatchRequestIds.push_back(dispatcher.QueueMissionPause(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::MissionForward:
        ConsoleUI::AddLog(
            requestPrefix +
            "MISSION_FORWARD legacy MDHK command=20 param1=0 awaiting ACK");
        submission.dispatchRequestIds.push_back(dispatcher.QueueMissionForward(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::MissionBackward:
        ConsoleUI::AddLog(
            requestPrefix +
            "MISSION_BACKWARD legacy MDHK command=20 param1=1 awaiting ACK");
        submission.dispatchRequestIds.push_back(dispatcher.QueueMissionBackward(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::ChangeSpeed:
        if (request.parameters.advancedSpeedMetersPerSecond < 1.0f ||
            request.parameters.advancedSpeedMetersPerSecond > 5.0f)
        {
            submission.error = "speed must be within [1, 5] m/s";
            return submission;
        }
        ConsoleUI::AddLog(requestPrefix + "CHANGE_SPEED MAV_CMD_USER_2 speed=" +
            std::to_string(request.parameters.advancedSpeedMetersPerSecond));
        submission.dispatchRequestIds.push_back(dispatcher.QueueChangeSpeed(
            commandTarget.systemID, commandTarget.componentID,
            request.parameters.advancedSpeedMetersPerSecond));
        submission.accepted = true;
        return submission;

    case DroneCommandType::TriggerSmoke:
        ConsoleUI::AddLog(requestPrefix +
            "TRIGGER_SMOKE relay=0 pulse=1000ms (automatic reset)");
        submission.dispatchRequestIds.push_back(dispatcher.QueueTriggerSmoke(
            commandTarget.systemID, commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::StopTelemetry:
        ConsoleUI::AddLog(requestPrefix +
            "STOP_TELEMETRY SET_MESSAGE_INTERVAL=-1 ids=1,24,33,30,187; heartbeat/ACK preserved");
        submission.dispatchRequestIds.push_back(dispatcher.QueueStopTelemetry(
            commandTarget.systemID, commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::ResumeTelemetry:
        ConsoleUI::AddLog(requestPrefix +
            "RESUME_TELEMETRY SET_MESSAGE_INTERVAL=1000000us ids=1,24,33,30,187; verify fresh #187");
        submission.dispatchRequestIds.push_back(dispatcher.QueueResumeTelemetry(
            commandTarget.systemID, commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::DisableLedOutput:
    case DroneCommandType::RestoreLedOutput:
    {
        const bool disabled = request.commandType == DroneCommandType::DisableLedOutput;
        ConsoleUI::AddLog(requestPrefix +
            (disabled ? "DISABLE_LED_OUTPUT" : "RESTORE_LED_OUTPUT") +
            " MAV_CMD_USER_3");
        submission.dispatchRequestIds.push_back(dispatcher.QueueSetLedOutputDisabled(
            commandTarget.systemID, commandTarget.componentID, disabled));
        submission.accepted = true;
        return submission;
    }

    case DroneCommandType::Wake:
        ConsoleUI::AddLog(requestPrefix +
            "WAKE PREFLIGHT_REBOOT_SHUTDOWN autopilot_action=4");
        submission.dispatchRequestIds.push_back(dispatcher.QueuePowerControl(
            commandTarget.systemID, commandTarget.componentID, 4, false));
        submission.accepted = true;
        return submission;

    case DroneCommandType::Standby:
    case DroneCommandType::Reboot:
    case DroneCommandType::Shutdown:
    {
        if (commandTarget.armed)
        {
            submission.error = "power command blocked while vehicle is armed";
            ConsoleUI::AddLog(requestPrefix + submission.error);
            return submission;
        }
        if (request.commandType == DroneCommandType::Shutdown)
        {
            ConsoleUI::AddLog(requestPrefix +
                "SHUTDOWN legacy relay=1 latch (vehicle confirmed disarmed)");
            submission.dispatchRequestIds.push_back(dispatcher.QueueShutdown(
                commandTarget.systemID, commandTarget.componentID));
        }
        else
        {
            const std::uint8_t action = request.commandType == DroneCommandType::Standby
                ? 2 : 1;
            ConsoleUI::AddLog(requestPrefix +
                (request.commandType == DroneCommandType::Standby ? "STANDBY" : "REBOOT") +
                " PREFLIGHT_REBOOT_SHUTDOWN (vehicle confirmed disarmed)");
            submission.dispatchRequestIds.push_back(dispatcher.QueuePowerControl(
                commandTarget.systemID, commandTarget.componentID, action, true));
        }
        submission.accepted = true;
        return submission;
    }

    case DroneCommandType::SetLedColor:
        ConsoleUI::AddLog(
            requestPrefix + "SetLedColor RemoteEndpoint=" +
            config.remoteIp + ":" + std::to_string(config.remotePort) +
            " RGB=(" + std::to_string(request.parameters.red) + "," +
            std::to_string(request.parameters.green) + "," +
            std::to_string(request.parameters.blue) + ")");
        submission.dispatchRequestIds.push_back(dispatcher.QueueSetLedColor(
            commandTarget.systemID,
            commandTarget.componentID,
            request.parameters.red,
            request.parameters.green,
            request.parameters.blue));
        submission.accepted = true;
        return submission;

    case DroneCommandType::TimeStart:
        if (request.parameters.timeControlMask != 1 ||
            request.parameters.scheduledHour > 23 ||
            request.parameters.scheduledMinute > 59 ||
            request.parameters.scheduledSecond > 59)
        {
            ConsoleUI::AddLog(
                requestPrefix + "TIME_START blocked: invalid mask or local time");
            submission.error = "invalid scheduled time control parameters";
            return submission;
        }

        ConsoleUI::AddLog(
            requestPrefix + "MP_TIME_CONTROL START RemoteEndpoint=" +
            config.remoteIp + ":" + std::to_string(config.remotePort) +
            " control=1 mask=" +
            std::to_string(request.parameters.timeControlMask) +
            " local_time=" +
            std::to_string(request.parameters.scheduledHour) + ":" +
            std::to_string(request.parameters.scheduledMinute) + ":" +
            std::to_string(request.parameters.scheduledSecond));
        submission.dispatchRequestIds.push_back(dispatcher.QueueTimeControl(
            commandTarget.systemID,
            commandTarget.componentID,
            1,
            request.parameters.timeControlMask,
            request.parameters.scheduledHour,
            request.parameters.scheduledMinute,
            request.parameters.scheduledSecond));
        submission.accepted = true;
        return submission;

    case DroneCommandType::TimeStop:
        if (request.parameters.timeControlMask != 1)
        {
            ConsoleUI::AddLog(
                requestPrefix + "TIME_STOP blocked: invalid mask");
            submission.error = "invalid scheduled time control mask";
            return submission;
        }

        ConsoleUI::AddLog(
            requestPrefix + "TIME_STOP RemoteEndpoint=" +
            config.remoteIp + ":" + std::to_string(config.remotePort) +
            " Actions=MP_TIME_CONTROL_CANCEL,MAV_CMD_USER_1(0) mask=" +
            std::to_string(request.parameters.timeControlMask));
        submission.dispatchRequestIds.push_back(dispatcher.QueueEndDanceStep(
            commandTarget.systemID,
            commandTarget.componentID,
            request.parameters.timeControlMask));
        submission.accepted = true;
        return submission;

    case DroneCommandType::ExecuteDanceStep:
        ConsoleUI::AddLog(
            requestPrefix + "EXECUTE_DANCE_STEP RemoteEndpoint=" +
            config.remoteIp + ":" + std::to_string(config.remotePort) +
            " Actions=SET_MODE_AUTO,MAV_CMD_USER_1(1)");
        submission.dispatchRequestIds.push_back(dispatcher.QueueExecuteDanceStep(
            commandTarget.systemID,
            commandTarget.componentID));
        submission.accepted = true;
        return submission;

    case DroneCommandType::EndDanceStep:
        if (request.parameters.timeControlMask != 1)
        {
            ConsoleUI::AddLog(
                requestPrefix + "END_DANCE_STEP blocked: invalid mask");
            submission.error = "invalid scheduled time control mask";
            return submission;
        }

        ConsoleUI::AddLog(
            requestPrefix + "END_DANCE_STEP RemoteEndpoint=" +
            config.remoteIp + ":" + std::to_string(config.remotePort) +
            " Actions=MP_TIME_CONTROL_CANCEL,MAV_CMD_USER_1(0)");
        submission.dispatchRequestIds.push_back(dispatcher.QueueEndDanceStep(
            commandTarget.systemID,
            commandTarget.componentID,
            request.parameters.timeControlMask));
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
        identity.sourceIp != config.remoteIp ||
        identity.sourcePort != config.remotePort)
    {
        return false;
    }

    config.discoveredSystemId = identity.systemId;
    config.discoveredComponentId = identity.componentId;
    config.remoteIp = identity.sourceIp;
    config.remotePort = identity.sourcePort;
    if (mavFtpClient && !mavFtpClient->IsBusy())
    {
        dronegcs::mavftp::PeerIdentity peer;
        peer.droneId = config.droneId;
        peer.remoteIp = config.remoteIp;
        peer.remotePort = static_cast<std::uint16_t>(config.remotePort);
        peer.systemId = config.discoveredSystemId;
        peer.componentId = config.discoveredComponentId;
        (void)mavFtpClient->RebindPeer(peer);
    }

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
        identity.sourceIp != config.remoteIp ||
        identity.sourcePort != config.remotePort)
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
    dispatcher.HandleHeartbeat(message);
    if (previousMode != workingData.mode || previousModeName != workingData.modeName)
    {
        ConsoleUI::AddLog(
            "[HEARTBEAT] " + config.droneId +
            " Mode=" + workingData.modeName +
            " custom_mode=" + std::to_string(workingData.mode) +
            " endpoint=" + config.remoteIp + ":" +
            std::to_string(config.remotePort));
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
        identity.sourceIp != config.remoteIp ||
        identity.sourcePort != config.remotePort)
    {
        return false;
    }

    switch (message.msgid)
    {
    case MAVLINK_MSG_ID_STATUSTEXT:
    {
        mavlink_statustext_t status{};
        mavlink_msg_statustext_decode(&message, &status);
        ConsoleUI::AddLog(
            SessionPrefix(config.droneId) +
            "STATUSTEXT severity=" + MavSeverityName(status.severity) +
            "(" + std::to_string(static_cast<unsigned int>(status.severity)) +
            ") text=" + StatusTextString(status));
        return true;
    }
    case MAVLINK_MSG_ID_ATTITUDE:
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
    case MAVLINK_MSG_ID_GPS_RAW_INT:
    case MAVLINK_MSG_ID_SYS_STATUS:
    case MAVLINK_MSG_ID_AUTOPILOT_VERSION:
    case MAVLINK_MSG_ID_MDHK_STATUS:
        parser.Parse(message, workingData);
        dataManager.Update(workingData);
        dispatcher.HandleTelemetryMessage(message.msgid);
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
        identity.sourceIp != config.remoteIp ||
        identity.sourcePort != config.remotePort)
    {
        return false;
    }

    parser.Parse(message, workingData);
    dataManager.Update(workingData);
    dispatcher.HandleAck(message);
    return true;
}

bool DroneSession::ApplyDiscoveredFtpMessage(
    const mavlink_message_t& message,
    const DiscoveredDroneIdentity& identity)
{
    if (!mavFtpClient || message.msgid != MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL ||
        message.sysid != config.discoveredSystemId ||
        message.compid != config.discoveredComponentId ||
        identity.systemId != config.discoveredSystemId ||
        identity.componentId != config.discoveredComponentId ||
        identity.sourceIp != config.remoteIp ||
        identity.sourcePort != config.remotePort)
    {
        return false;
    }

    mavlink_file_transfer_protocol_t ftp{};
    mavlink_msg_file_transfer_protocol_decode(&message, &ftp);
    if (ftp.target_system != 255 ||
        ftp.target_component != MAV_COMP_ID_MISSIONPLANNER)
    {
        return false;
    }
    dronegcs::mavftp::PeerIdentity source;
    source.droneId = config.droneId;
    source.remoteIp = identity.sourceIp;
    source.remotePort = identity.sourcePort;
    source.systemId = identity.systemId;
    source.componentId = identity.componentId;
    return mavFtpClient->HandleResponse(
        source, ftp.payload, sizeof(ftp.payload));
}

bool DroneSession::ApplyDiscoveredMissionMessage(
    const mavlink_message_t& message,
    const DiscoveredDroneIdentity& identity)
{
    const bool supportedMessage =
        message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_INT ||
        message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST ||
        message.msgid == MAVLINK_MSG_ID_MISSION_ACK ||
        message.msgid == MAVLINK_MSG_ID_MISSION_COUNT ||
        message.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT;
    if (!supportedMessage ||
        message.sysid != config.discoveredSystemId ||
        message.compid != config.discoveredComponentId ||
        identity.systemId != config.discoveredSystemId ||
        identity.componentId != config.discoveredComponentId ||
        identity.sourceIp != config.remoteIp ||
        identity.sourcePort != config.remotePort)
    {
        return false;
    }
    bool consumed = false;
    if (message.msgid == MAVLINK_MSG_ID_MISSION_COUNT && mavFtpClient)
    {
        mavlink_mission_count_t count{};
        mavlink_msg_mission_count_decode(&message, &count);
        if (count.target_system == 255 &&
            count.target_component == MAV_COMP_ID_MISSIONPLANNER &&
            count.mission_type == MAV_MISSION_TYPE_MISSION)
        {
            receivedMissionCount = count.count;
            consumed = true;
        }
    }
    if (missionVerificationSink && config.missionUploadServiceEnabled)
    {
        consumed = missionVerificationSink->HandleMissionMessage(
            message, GetMissionVerificationPeerIdentity(),
            std::chrono::steady_clock::now()) || consumed;
    }
    return consumed;
}

bool DroneSession::SendMavFtpPayload(
    const std::array<std::uint8_t, dronegcs::mavftp::PayloadBytes>& payload)
{
    if (!started || !config.mavFtpServiceEnabled ||
        !dataManager.GetFlightData().online)
    {
        return false;
    }
    mavlink_message_t message{};
    std::uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    mavlink_msg_file_transfer_protocol_pack(
        255, MAV_COMP_ID_MISSIONPLANNER, &message,
        0, config.discoveredSystemId, config.discoveredComponentId,
        payload.data());
    const std::uint16_t length = mavlink_msg_to_send_buffer(buffer, &message);
    return udp.Send(buffer, length);
}

bool DroneSession::SendMissionRequestList()
{
    if (!started || !config.mavFtpServiceEnabled ||
        !dataManager.GetFlightData().online)
    {
        return false;
    }
    receivedMissionCount.reset();
    mavlink_message_t message{};
    std::uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    mavlink_msg_mission_request_list_pack(
        255, MAV_COMP_ID_MISSIONPLANNER, &message,
        config.discoveredSystemId, config.discoveredComponentId,
        MAV_MISSION_TYPE_MISSION);
    const std::uint16_t length = mavlink_msg_to_send_buffer(buffer, &message);
    return udp.Send(buffer, length);
}

namespace
{
    bool SendMissionWire(UdpClient& udp, mavlink_message_t& message,
        std::size_t& wireBytes)
    {
        std::uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
        const std::uint16_t length = mavlink_msg_to_send_buffer(buffer, &message);
        wireBytes = length;
        return udp.Send(buffer, length);
    }
}

bool DroneSession::SendMissionRequestList(std::size_t& wireBytes)
{
    wireBytes = 0;
    if (!started || !config.missionUploadServiceEnabled ||
        !dataManager.GetFlightData().online)
        return false;
    mavlink_message_t message{};
    mavlink_msg_mission_request_list_pack(
        255, MAV_COMP_ID_MISSIONPLANNER, &message,
        config.discoveredSystemId, config.discoveredComponentId,
        MAV_MISSION_TYPE_MISSION);
    return SendMissionWire(udp, message, wireBytes);
}

bool DroneSession::SendMissionRequestInt(
    const std::uint16_t sequence,
    std::size_t& wireBytes)
{
    wireBytes = 0;
    if (!started || !config.missionUploadServiceEnabled ||
        !dataManager.GetFlightData().online)
        return false;
    mavlink_message_t message{};
    mavlink_msg_mission_request_int_pack(
        255, MAV_COMP_ID_MISSIONPLANNER, &message,
        config.discoveredSystemId, config.discoveredComponentId,
        sequence, MAV_MISSION_TYPE_MISSION);
    return SendMissionWire(udp, message, wireBytes);
}

void DroneSession::SetMissionVerificationMessageSink(
    IMissionVerificationMessageSink* const sink)
{
    missionVerificationSink = sink;
}

MissionVerificationPeerIdentity DroneSession::GetMissionVerificationPeerIdentity() const
{
    MissionVerificationPeerIdentity peer;
    peer.droneId = config.droneId;
    peer.remoteIp = config.remoteIp;
    peer.remotePort = static_cast<std::uint16_t>(config.remotePort);
    peer.systemId = config.discoveredSystemId;
    peer.componentId = config.discoveredComponentId;
    return peer;
}

dronegcs::mavftp::Client* DroneSession::GetMavFtpClient()
{
    return mavFtpClient.get();
}

const dronegcs::mavftp::Client* DroneSession::GetMavFtpClient() const
{
    return mavFtpClient.get();
}

std::optional<std::uint16_t> DroneSession::ConsumeMissionCount()
{
    const std::optional<std::uint16_t> result = receivedMissionCount;
    receivedMissionCount.reset();
    return result;
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

void DroneSession::ProcessDispatchResults(
    std::chrono::steady_clock::time_point now)
{
    dispatcher.Tick(now);

    const std::vector<CommandDispatchResult> results =
        dispatcher.ConsumeResults();

    for (const CommandDispatchResult& result : results)
    {
        completedCommandResults.push_back(result);
        std::string ackDiagnostic;
        if (result.modeConfirmationRequired)
        {
            ackDiagnostic =
                " mode_confirmation=" +
                std::string(result.outcome == CommandOutcome::Accepted
                    ? "CONFIRMED"
                    : "NOT_CONFIRMED") +
                " custom_mode=" + std::to_string(
                    static_cast<unsigned int>(result.expectedCustomMode));
        }
        else if (result.outcome == CommandOutcome::Accepted ||
            result.outcome == CommandOutcome::Rejected)
        {
            ackDiagnostic =
                " ack=" + std::string(MavResultName(result.ackResult)) +
                "(" + std::to_string(
                    static_cast<unsigned int>(result.ackResult)) + ")";
        }
        ConsoleUI::AddLog(
            SessionPrefix(config.droneId) +
            "CMD request=" + std::to_string(result.requestId) +
            " command=" + std::to_string(result.command) +
            " result=" + CommandOutcomeName(result.outcome) +
            ackDiagnostic +
            " attempts=" + std::to_string(result.attempts));
    }
}
