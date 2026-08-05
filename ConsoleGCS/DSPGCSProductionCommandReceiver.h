#pragma once

#include "DSPGCSProtocol.h"
#include "IDSPGCSTransport.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class CommandManager;

struct DSPGCSRealCommandValidation
{
    bool allowed = false;
    std::string errorCode;
    std::string errorMessage;
};

/**
 * Production DSP-GCS command ingress adapter.
 *
 * The transport thread only decodes and queues DTOs. ProcessPending must be
 * called by the ConsoleGCS main thread, which is the only thread allowed to
 * submit to CommandManager/DroneManager.
 */
class DSPGCSProductionCommandReceiver
{
public:
    using ResultSender =
        std::function<bool(const dspgcs::CommandResultBatchDTO&)>;

    void EnqueueFrame(dspgcs::TransportFrame frame);
    void SetRealCommandEnabled(bool enabled);
    static DSPGCSRealCommandValidation ValidateRealCommand(
        const dspgcs::CommandRequestDTO& request);
    std::size_t ProcessPending(
        CommandManager& commandManager,
        const ResultSender& sendResult);

    std::size_t GetReceivedCommandCount() const;

private:
    struct QueuedCommand
    {
        dspgcs::MessageEnvelope envelope;
        dspgcs::CommandRequestDTO request;
    };

    static dspgcs::CommandResultBatchDTO MakeRejectedBatch(
        const dspgcs::CommandRequestDTO& request,
        const std::string& code,
        const std::string& message);

    mutable std::mutex mutex_;
    std::deque<QueuedCommand> pending_;
    std::unordered_set<std::string> knownCommandIds_;
    std::unordered_map<std::uint64_t, dspgcs::CommandRequestDTO>
        activeCommands_;
    bool realCommandEnabled_ = false;
    std::size_t receivedCommandCount_ = 0;
};
