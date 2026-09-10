/**
 * @brief 接收 UE 发来的 DSP-GCS 领域命令并向 CommandManager 提交。
 *
 * 分类：Production。
 * 输入：DSP Transport Frame；输出：命令队列、最终/中间 CommandResultBatch。
 * 调用链：UE CommandProvider -> DSP Transport -> Receiver -> CommandManager -> DroneSession。
 * 关键约束：目标使用稳定 DroneID，真实命令仍受安全模式和高度上限约束。
 */
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
        const ResultSender& sendResult,
        const std::function<void()>& serviceIngress = {});

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
