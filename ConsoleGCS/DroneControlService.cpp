/**
 * @brief 实现旧文本命令服务的请求缓存、状态回复和命令完成等待。
 *
 * 分类：Legacy / Compatibility；保留用于旧链兼容，不参与当前多机 Production 调用链。
 */
#include "DroneControlService.h"

#include "ConsoleUI.h"
#include "SafetyConfig.h"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

DroneControlService::DroneControlService(
    DataManager& dataManager,
    CommandDispatcher& dispatcher,
    UdpCommandServer& commandServer)
    : dataManager(dataManager),
      dispatcher(dispatcher),
      commandServer(commandServer)
{
    lastStatusPublish = std::chrono::steady_clock::now();
}

void DroneControlService::Poll()
{
    char buffer[SafetyConfig::MAX_COMMAND_PACKET_BYTES]{};
    sockaddr_in sender{};

    const int received = commandServer.Receive(
        buffer,
        static_cast<int>(sizeof(buffer)),
        sender);

    if (received <= 0)
    {
        return;
    }

    if (!commandServer.IsAllowedClient(sender))
    {
        commandServer.Send(
            CommandProtocol::Error(
                "UNKNOWN",
                "UNKNOWN",
                "CLIENT_NOT_ALLOWED"),
            sender);
        return;
    }

    CommandRequest request;
    std::string error;

    if (!CommandProtocol::Parse(
        buffer,
        received,
        request,
        error))
    {
        commandServer.Send(
            CommandProtocol::Error(
                request.requestId.empty()
                    ? "UNKNOWN"
                    : request.requestId,
                request.command.empty()
                    ? "UNKNOWN"
                    : request.command,
                error),
            sender);
        return;
    }

    HandleRequest(request, sender);
}

void DroneControlService::Tick()
{
    PublishFlightState();

    if (!pending.active)
    {
        return;
    }

    if (pending.expectedMode >= 0)
    {
        const FlightData data = dataManager.GetFlightData();

        if (data.online &&
            data.mode == pending.expectedMode)
        {
            CompletePending(true, "MODE_CONFIRMED");
            return;
        }
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() -
            pending.startedAt).count();

    if (elapsed >= SafetyConfig::COMMAND_ACK_TIMEOUT_MS)
    {
        CompletePending(false, "APM_CONFIRMATION_TIMEOUT");
    }
}

void DroneControlService::HandleDispatchResults(
    const std::vector<CommandDispatchResult>& results)
{
    if (!pending.active)
    {
        return;
    }

    for (const CommandDispatchResult& result : results)
    {
        if (result.requestId != pending.dispatchId)
        {
            continue;
        }

        switch (result.outcome)
        {
        case CommandOutcome::Accepted:
            CompletePending(true, "APM_ACCEPTED");
            break;

        case CommandOutcome::Sent:
            if (pending.expectedMode < 0)
            {
                CompletePending(true, "SENT");
            }
            break;

        case CommandOutcome::Rejected:
            CompletePending(
                false,
                "APM_REJECTED_" +
                std::to_string(result.ackResult));
            break;

        case CommandOutcome::Timeout:
            CompletePending(false, "APM_ACK_TIMEOUT");
            break;

        case CommandOutcome::SendFailed:
            CompletePending(false, "MAVLINK_SEND_FAILED");
            break;

        case CommandOutcome::Superseded:
            CompletePending(false, "COMMAND_SUPERSEDED");
            break;

        case CommandOutcome::QueueFull:
            CompletePending(false, "COMMAND_QUEUE_FULL");
            break;
        }

        return;
    }
}

void DroneControlService::HandleRequest(
    const CommandRequest& request,
    const sockaddr_in& sender)
{
    const auto cached = responseCache.find(request.requestId);

    if (cached != responseCache.end())
    {
        commandServer.Send(cached->second, sender);
        return;
    }

    if (request.command == "PING")
    {
        hasStatusSubscriber = true;
        statusSubscriber = sender;

        ReplyAndCache(
            request.requestId,
            CommandProtocol::Pong(
                request.requestId,
                SafetyConfig::SAFE_TEST_MODE),
            sender);
        return;
    }

    if (SafetyConfig::SAFE_TEST_MODE)
    {
        ReplyAndCache(
            request.requestId,
            CommandProtocol::Ack(
                request.requestId,
                request.command,
                "SIMULATED_SAFE_TEST_MODE"),
            sender);
        return;
    }

    if (pending.active)
    {
        ReplyAndCache(
            request.requestId,
            CommandProtocol::Error(
                request.requestId,
                request.command,
                "ANOTHER_UE_COMMAND_PENDING"),
            sender);
        return;
    }

    const FlightData data = dataManager.GetFlightData();

    if (!data.online)
    {
        ReplyAndCache(
            request.requestId,
            CommandProtocol::Error(
                request.requestId,
                request.command,
                "VEHICLE_OFFLINE"),
            sender);
        return;
    }

    if (request.command == "DISARM" &&
        data.armed &&
        (!data.altitudeValid ||
            data.relativeAltitude > 0.3f))
    {
        ReplyAndCache(
            request.requestId,
            CommandProtocol::Error(
                request.requestId,
                request.command,
                "LANDING_NOT_CONFIRMED"),
            sender);
        return;
    }

    std::string error;

    if (!QueueRealCommand(request, sender, error))
    {
        ReplyAndCache(
            request.requestId,
            CommandProtocol::Error(
                request.requestId,
                request.command,
                error),
            sender);
    }
}

bool DroneControlService::QueueRealCommand(
    const CommandRequest& request,
    const sockaddr_in& sender,
    std::string& error)
{
    const FlightData data = dataManager.GetFlightData();
    std::uint64_t dispatchId = 0;
    int expectedMode = -1;

    if (request.command == "ARM")
    {
        dispatchId = dispatcher.QueueArm(
            data.systemID,
            data.componentID);
    }
    else if (request.command == "DISARM")
    {
        dispatchId = dispatcher.QueueDisarm(
            data.systemID,
            data.componentID);
    }
    else if (request.command == "GUIDED")
    {
        dispatchId = dispatcher.QueueGuided(
            data.systemID,
            data.componentID);
        expectedMode = 4;
    }
    else if (request.command == "TAKEOFF")
    {
        float altitude = 0.0f;

        if (!ParseTakeoffAltitude(request, altitude))
        {
            error = "BAD_TAKEOFF_ALTITUDE";
            return false;
        }

        dispatchId = dispatcher.QueueTakeoff(
            data.systemID,
            data.componentID,
            altitude);
    }
    else if (request.command == "LAND" ||
        request.command == "STOP")
    {
        dispatchId = dispatcher.QueueLand(
            data.systemID,
            data.componentID);
    }
    else if (request.command == "RTL")
    {
        dispatchId = dispatcher.QueueRTL(
            data.systemID,
            data.componentID);
    }
    else if (request.command == "LOITER" ||
        request.command == "HOVER")
    {
        dispatchId = dispatcher.QueueLoiter(
            data.systemID,
            data.componentID);
        expectedMode = 5;
    }
    else
    {
        error = "UNSUPPORTED_COMMAND";
        return false;
    }

    pending.active = true;
    pending.dispatchId = dispatchId;
    pending.requestId = request.requestId;
    pending.command = request.command;
    pending.expectedMode = expectedMode;
    pending.recipient = sender;
    pending.startedAt = std::chrono::steady_clock::now();

    ConsoleUI::AddLog(
        "UE command queued: " + request.command);
    return true;
}

void DroneControlService::CompletePending(
    bool success,
    const std::string& detail)
{
    if (!pending.active)
    {
        return;
    }

    const std::string response = success
        ? CommandProtocol::Ack(
            pending.requestId,
            pending.command,
            detail)
        : CommandProtocol::Error(
            pending.requestId,
            pending.command,
            detail);

    ReplyAndCache(
        pending.requestId,
        response,
        pending.recipient);

    ConsoleUI::AddLog(
        "UE command " +
        pending.command +
        ": " +
        detail);

    pending = {};
}

void DroneControlService::ReplyAndCache(
    const std::string& requestId,
    const std::string& response,
    const sockaddr_in& recipient)
{
    if (responseCache.size() >= 256)
    {
        responseCache.clear();
    }

    responseCache[requestId] = response;
    commandServer.Send(response, recipient);
}

void DroneControlService::PublishFlightState()
{
    if (!hasStatusSubscriber)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastStatusPublish).count();

    if (elapsed < 500)
    {
        return;
    }

    commandServer.Send(
        BuildFlightStateMessage(),
        statusSubscriber);
    lastStatusPublish = now;
}

std::string DroneControlService::BuildFlightStateMessage() const
{
    const FlightData data = dataManager.GetFlightData();
    std::ostringstream stream;

    stream << std::fixed << std::setprecision(7)
        << "GCS1|STATE|STATE"
        << "|ONLINE=" << (data.online ? 1 : 0)
        << "|SYSID=" << static_cast<int>(data.systemID)
        << "|COMPID=" << static_cast<int>(data.componentID)
        << "|ARMED=" << (data.armed ? 1 : 0)
        << "|MODE=" << data.modeName
        << "|GPS=" << static_cast<int>(data.satellitesVisible)
        << "|LAT=" << data.latitude
        << "|LON=" << data.longitude;

    stream << std::setprecision(2)
        << "|ALT=" << data.altitude
        << "|RELALT=" << data.relativeAltitude
        << "|SPD=" << data.groundSpeed
        << "|BAT=" << data.batteryRemaining
        << "|ROLL=" << data.roll
        << "|PITCH=" << data.pitch
        << "|YAW=" << data.yaw;

    return stream.str();
}

bool DroneControlService::ParseTakeoffAltitude(
    const CommandRequest& request,
    float& altitude)
{
    altitude =
        SafetyConfig::DEFAULT_TAKEOFF_ALTITUDE_METERS;

    if (request.arguments.empty())
    {
        return true;
    }

    const std::string prefix = "ALT=";
    const std::string& argument =
        request.arguments.front();

    if (argument.rfind(prefix, 0) != 0)
    {
        return false;
    }

    char* end = nullptr;
    const float parsed = std::strtof(
        argument.c_str() + prefix.size(),
        &end);

    if (end == argument.c_str() + prefix.size() ||
        *end != '\0' ||
        !std::isfinite(parsed) ||
        parsed < 0.5f ||
        parsed > 20.0f)
    {
        return false;
    }

    altitude = parsed;
    return true;
}
