/**
 * @brief 将 Console Fleet 快照编码为 DSP-GCS 遥测帧并发送命令结果。
 *
 * 分类：Production。
 * 输入：DroneManager 全量 Session 与 CommandResult；输出：分片 Telemetry Snapshot/Result Frame。
 * 调用链：ConsoleGCSApplication -> Provider -> DSP Transport -> Unreal Production Provider。
 * 关键约束：大 Fleet 必须完整分片，不能发送不可重组的半个 Snapshot。
 */
#pragma once

#include "DSPGCSProtocol.h"
#include "DSPGCSTransportConfig.h"
#include "IDSPGCSTransport.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class DroneManager;
class DroneSession;

class DSPGCSProductionTelemetryProvider
{
public:
    void SetReceiveHandler(dspgcs::TransportReceiveHandler handler);
    bool Start(
        const dspgcs::DSPGCSTransportConfig& transportConfig,
        std::string& error);
    void Stop();
    bool PublishSnapshot(const DroneManager& droneManager);
    bool SendCommandResult(
        const dspgcs::CommandResultBatchDTO& resultBatch);
    bool IsRunning() const;

private:
    std::unique_ptr<dspgcs::IDSPGCSTransport> transport_;
    dspgcs::TransportReceiveHandler receiveHandler_;
    std::uint64_t sequence_ = 0;
    std::uint64_t fleetRevision_ = 0;
    std::string sessionId_;
};
