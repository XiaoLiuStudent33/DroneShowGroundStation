/**
 * @brief 实现 DSP 命令解码、校验、领域映射和结果回传准备。
 *
 * 分类：Production。
 * 本文件不直接选择 UDP Endpoint；CommandManager 经 DroneID 找到原 DroneSession 后发送。
 */
#include "DSPGCSProductionCommandReceiver.h"

#include "CommandManager.h"
#include "ConsoleUI.h"
#include "SafetyConfig.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace
{
    std::int64_t UtcNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool TryToInternalType(
        const dspgcs::CommandType type,
        DroneCommandType& result)
    {
        switch (type)
        {
        case dspgcs::CommandType::Arm: result = DroneCommandType::Arm; return true;
        case dspgcs::CommandType::Disarm: result = DroneCommandType::Disarm; return true;
        case dspgcs::CommandType::Takeoff: result = DroneCommandType::Takeoff; return true;
        case dspgcs::CommandType::Land: result = DroneCommandType::Land; return true;
        case dspgcs::CommandType::RTL: result = DroneCommandType::RTL; return true;
        case dspgcs::CommandType::SetMode: result = DroneCommandType::SetMode; return true;
        case dspgcs::CommandType::HoverTest: result = DroneCommandType::HoverTest; return true;
        }
        return false;
    }

    bool IsPhase6RealCommand(const dspgcs::CommandType type)
    {
        switch (type)
        {
        case dspgcs::CommandType::Arm:
        case dspgcs::CommandType::Disarm:
        case dspgcs::CommandType::Takeoff:
        case dspgcs::CommandType::Land:
        case dspgcs::CommandType::RTL:
        case dspgcs::CommandType::SetMode:
        case dspgcs::CommandType::HoverTest:
            return true;
        default:
            return false;
        }
    }

    dspgcs::AckState ToProtocolState(const DroneCommandTargetState state)
    {
        switch (state)
        {
        case DroneCommandTargetState::Pending: return dspgcs::AckState::Queued;
        case DroneCommandTargetState::Sent: return dspgcs::AckState::Sent;
        case DroneCommandTargetState::Accepted: return dspgcs::AckState::Accepted;
        case DroneCommandTargetState::Rejected: return dspgcs::AckState::Rejected;
        case DroneCommandTargetState::Timeout: return dspgcs::AckState::TimedOut;
        case DroneCommandTargetState::Failed: return dspgcs::AckState::Failed;
        case DroneCommandTargetState::Simulated: return dspgcs::AckState::Simulated;
        }
        return dspgcs::AckState::Failed;
    }

    std::string SourceName(const dspgcs::CommandRequestDTO& request)
    {
        std::string result = request.source.sourceType;
        if (!request.source.sourceId.empty())
        {
            if (!result.empty()) result += ":";
            result += request.source.sourceId;
        }
        return result.empty() ? "DSPGCS" : result;
    }

    dspgcs::CommandResultBatchDTO ToResultBatch(
        const dspgcs::CommandRequestDTO& request,
        const ManagedDroneCommand& command)
    {
        dspgcs::CommandResultBatchDTO batch;
        batch.commandId = request.commandId;
        batch.resultSequence = 1;
        batch.final = true;
        batch.requestedCount =
            static_cast<std::uint32_t>(request.targetDroneIds.size());

        std::uint32_t simulatedCount = 0;
        std::uint32_t succeededCount = 0;
        for (const DroneCommandTargetResult& target : command.targetResults)
        {
            dspgcs::DroneCommandResultDTO result;
            result.commandId = request.commandId;
            result.droneId = target.droneId;
            result.ackState = ToProtocolState(target.state);
            result.attempts = static_cast<std::uint16_t>(target.attempts);
            result.updatedTimestampUtcMs = UtcNowMs();
            result.final = true;

            if (target.state == DroneCommandTargetState::Simulated)
            {
                ++simulatedCount;
                // Protocol v1 already provides the optional message field.
                // Keep the error code empty and use the message for the MVP
                // execution state, without changing the DSP-GCS schema.
                result.error.message = target.detail;
            }
            else if (target.state == DroneCommandTargetState::Accepted ||
                target.state == DroneCommandTargetState::Sent)
            {
                ++succeededCount;
            }
            else if (target.state == DroneCommandTargetState::Timeout)
            {
                ++batch.failedCount;
                result.error.code = "COMMAND_TIMEOUT";
                result.error.message = target.detail;
                result.timeout.stage = "CommandManager";
            }
            else if (target.state == DroneCommandTargetState::Rejected ||
                target.state == DroneCommandTargetState::Failed)
            {
                ++batch.failedCount;
                result.error.code = target.state == DroneCommandTargetState::Rejected
                    ? "COMMAND_REJECTED"
                    : "COMMAND_FAILED";
                result.error.message = target.detail;
            }
            batch.results.push_back(std::move(result));
        }

        batch.acceptedCount = simulatedCount + succeededCount;
        batch.succeededCount = simulatedCount + succeededCount;
        if (simulatedCount == batch.requestedCount)
        {
            batch.aggregateState = dspgcs::AggregateState::Simulated;
        }
        else if (succeededCount == batch.requestedCount)
        {
            batch.aggregateState = dspgcs::AggregateState::Succeeded;
        }
        else if (simulatedCount > 0 || succeededCount > 0)
        {
            batch.aggregateState = dspgcs::AggregateState::PartialSuccess;
        }
        else
        {
            batch.aggregateState = dspgcs::AggregateState::Failed;
        }
        return batch;
    }
}

DSPGCSRealCommandValidation
DSPGCSProductionCommandReceiver::ValidateRealCommand(
    const dspgcs::CommandRequestDTO& request)
{
    DSPGCSRealCommandValidation validation;
    if (request.targetDroneIds.empty())
    {
        validation.errorCode = "REAL_COMMAND_TARGET_REQUIRED";
        validation.errorMessage = "Real flight requires at least one target";
        return validation;
    }
    if (!IsPhase6RealCommand(request.commandType))
    {
        validation.errorCode = "REAL_COMMAND_NOT_ALLOWED";
        validation.errorMessage =
            "CommandType is not allowed in Phase 7.1 real-command mode";
        return validation;
    }
    if (request.commandType == dspgcs::CommandType::SetMode &&
        !ArduCopterCustomMode::IsSupported(request.parameters.customMode))
    {
        validation.errorCode = "REAL_COMMAND_MODE_NOT_ALLOWED";
        validation.errorMessage =
            "Real SET_MODE permits ArduPilot Copter STABILIZE(0), AUTO(3), or GUIDED(4)";
        return validation;
    }

    float altitude = 0.0f;
    float altitudeLimit = 0.0f;
    if (request.commandType == dspgcs::CommandType::Takeoff)
    {
        altitude = request.parameters.takeoffAltitudeMeters;
        altitudeLimit = SafetyConfig::TAKEOFF_ALTITUDE_LIMIT_METERS;
    }
    else if (request.commandType == dspgcs::CommandType::HoverTest)
    {
        altitude = request.parameters.hoverAltitudeMeters;
        altitudeLimit = SafetyConfig::HOVER_ALTITUDE_LIMIT_METERS;
    }

    if ((request.commandType == dspgcs::CommandType::Takeoff ||
         request.commandType == dspgcs::CommandType::HoverTest) &&
        (altitude <= 0.0f ||
         altitude > altitudeLimit))
    {
        validation.errorCode = "REAL_COMMAND_ALTITUDE_LIMIT";
        validation.errorMessage =
            "Real flight altitude must be within (0, " +
            std::to_string(altitudeLimit) + "] meters";
        return validation;
    }

    validation.allowed = true;
    return validation;
}

void DSPGCSProductionCommandReceiver::EnqueueFrame(
    dspgcs::TransportFrame frame)
{
    dspgcs::MessageEnvelope envelope;
    std::vector<std::uint8_t> payload;
    if (!dspgcs::DecodeEnvelope(frame.bytes, envelope, payload) ||
        envelope.messageType != dspgcs::MessageType::CommandRequest)
    {
        return;
    }

    dspgcs::CommandRequestDTO request;
    if (!dspgcs::DecodeCommand(payload, request))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back({ std::move(envelope), std::move(request) });
    ++receivedCommandCount_;
}

void DSPGCSProductionCommandReceiver::SetRealCommandEnabled(
    const bool enabled)
{
    realCommandEnabled_ = enabled;
}

std::size_t DSPGCSProductionCommandReceiver::ProcessPending(
    CommandManager& commandManager,
    const ResultSender& sendResult)
{
    std::deque<QueuedCommand> commands;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands.swap(pending_);
    }

    const auto publishCompleted = [&]()
    {
        commandManager.Tick();
        std::vector<ManagedDroneCommand> completed =
            commandManager.ConsumeCompletedCommands();
        for (const ManagedDroneCommand& command : completed)
        {
            const auto active = activeCommands_.find(command.request.commandId);
            if (active == activeCommands_.end())
            {
                continue;
            }

            const dspgcs::CommandResultBatchDTO resultBatch =
                ToResultBatch(active->second, command);
            if (sendResult)
            {
                sendResult(resultBatch);
            }
            ConsoleUI::AddLog(
                "DSP-GCS Result CommandID=" + resultBatch.commandId +
                " Results=" + std::to_string(resultBatch.results.size()) +
                " Succeeded=" + std::to_string(resultBatch.succeededCount) +
                " Failed=" + std::to_string(resultBatch.failedCount));
            activeCommands_.erase(active);
        }
    };

    // Publish ACK/timeout results belonging to commands submitted on an
    // earlier main-loop iteration before accepting new work.
    publishCompleted();

    std::size_t processed = 0;
    for (QueuedCommand& queued : commands)
    {
        ++processed;
        const dspgcs::CommandRequestDTO& request = queued.request;
        const std::string source = SourceName(request);
        ConsoleUI::AddLog(
            "DSP-GCS CommandID=" + request.commandId +
            " Source=" + source +
            " Type=" + std::to_string(
                static_cast<std::uint16_t>(request.commandType)) +
            " Targets=" + std::to_string(request.targetDroneIds.size()));
        if (request.commandType == dspgcs::CommandType::HoverTest)
        {
            ConsoleUI::AddLog(
                "DSP-GCS HoverParameters Altitude=" +
                std::to_string(request.parameters.hoverAltitudeMeters) +
                "m Duration=" +
                std::to_string(request.parameters.hoverDurationSeconds) +
                "s");
        }

        dspgcs::CommandResultBatchDTO resultBatch;
        bool sendImmediateResult = true;
        if (!knownCommandIds_.insert(request.commandId).second)
        {
            resultBatch = MakeRejectedBatch(
                request,
                "DUPLICATE_COMMAND_ID",
                "CommandID has already been processed");
        }
        else if (realCommandEnabled_)
        {
            const DSPGCSRealCommandValidation validation =
                ValidateRealCommand(request);
            if (!validation.allowed)
            {
                resultBatch = MakeRejectedBatch(
                    request,
                    validation.errorCode,
                    validation.errorMessage);
            }
            else
            {
                DroneCommandType internalType;
                if (!TryToInternalType(request.commandType, internalType))
                {
                    resultBatch = MakeRejectedBatch(
                        request,
                        "UNSUPPORTED_COMMAND",
                        "CommandType is not supported by Phase 7.1");
                }
                else
                {
                    DroneCommandParameters parameters;
                    parameters.takeoffAltitudeMeters =
                        request.parameters.takeoffAltitudeMeters;
                    parameters.hoverAltitudeMeters =
                        request.parameters.hoverAltitudeMeters;
                    parameters.hoverDurationSeconds =
                        request.parameters.hoverDurationSeconds;
                    parameters.customMode = request.parameters.customMode;
                    parameters.startHoverTest = request.parameters.start;

                    const std::uint64_t internalCommandId = commandManager.Submit(
                        internalType,
                        request.targetDroneIds,
                        parameters,
                        source);
                    activeCommands_.emplace(internalCommandId, request);
                    sendImmediateResult = false;
                }
            }
        }
        else if (!SafetyConfig::SAFE_TEST_MODE || !request.safeTestMode)
        {
            resultBatch = MakeRejectedBatch(
                request,
                "REAL_COMMAND_DISABLED",
                "Real MAVLink commands are disabled and SAFE_TEST_MODE was not requested");
        }
        else
        {
            DroneCommandType internalType;
            if (!TryToInternalType(request.commandType, internalType))
            {
                resultBatch = MakeRejectedBatch(
                    request,
                    "UNSUPPORTED_COMMAND",
                    "CommandType is not supported by Phase 5.2B");
            }
            else
            {
                DroneCommandParameters parameters;
                parameters.takeoffAltitudeMeters =
                    request.parameters.takeoffAltitudeMeters;
                parameters.hoverAltitudeMeters =
                    request.parameters.hoverAltitudeMeters;
                parameters.hoverDurationSeconds =
                    request.parameters.hoverDurationSeconds;
                parameters.customMode = request.parameters.customMode;
                parameters.startHoverTest = request.parameters.start;

                const std::uint64_t internalCommandId = commandManager.Submit(
                    internalType,
                    request.targetDroneIds,
                    parameters,
                    source);
                activeCommands_.emplace(internalCommandId, request);
                sendImmediateResult = false;
            }
        }

        if (sendImmediateResult && sendResult)
        {
            sendResult(resultBatch);
        }
        if (sendImmediateResult)
        {
            ConsoleUI::AddLog(
                "DSP-GCS Result CommandID=" + request.commandId +
                " Results=" + std::to_string(resultBatch.results.size()) +
                " Succeeded=" + std::to_string(resultBatch.succeededCount) +
                " Failed=" + std::to_string(resultBatch.failedCount));
        }
    }

    // SAFE_TEST_MODE and immediately rejected Session submissions complete in
    // the same iteration. Real commands remain active until ACK or timeout.
    publishCompleted();
    return processed;
}

std::size_t DSPGCSProductionCommandReceiver::GetReceivedCommandCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return receivedCommandCount_;
}

dspgcs::CommandResultBatchDTO
DSPGCSProductionCommandReceiver::MakeRejectedBatch(
    const dspgcs::CommandRequestDTO& request,
    const std::string& code,
    const std::string& message)
{
    dspgcs::CommandResultBatchDTO batch;
    batch.commandId = request.commandId;
    batch.resultSequence = 1;
    batch.aggregateState = dspgcs::AggregateState::Failed;
    batch.final = true;
    batch.requestedCount =
        static_cast<std::uint32_t>(request.targetDroneIds.size());
    batch.failedCount = batch.requestedCount;
    for (const std::string& droneId : request.targetDroneIds)
    {
        dspgcs::DroneCommandResultDTO result;
        result.commandId = request.commandId;
        result.droneId = droneId;
        result.ackState = dspgcs::AckState::Rejected;
        result.error.code = code;
        result.error.message = message;
        result.updatedTimestampUtcMs = UtcNowMs();
        result.final = true;
        batch.results.push_back(std::move(result));
    }
    return batch;
}
