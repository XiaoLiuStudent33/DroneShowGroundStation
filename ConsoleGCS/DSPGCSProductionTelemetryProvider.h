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
