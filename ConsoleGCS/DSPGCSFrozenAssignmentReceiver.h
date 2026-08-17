#pragma once

#include "FrozenAssignmentStore.h"
#include "FrozenAssignmentTransactionAssembler.h"
#include "IDSPGCSTransport.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

class DroneManager;

struct DSPGCSFrozenAssignmentProcessStats
{
    std::size_t framesProcessed = 0;
    std::size_t transactionsAccepted = 0;
    std::size_t transactionsRejected = 0;
    std::size_t acknowledgementsSent = 0;
    std::size_t expiredTransactions = 0;
};

class DSPGCSFrozenAssignmentReceiver
{
public:
    using AckSender =
        std::function<bool(const dspgcs::FrozenAssignmentAckDTO&)>;
    using Clock = FrozenAssignmentTransactionAssembler::Clock;

    explicit DSPGCSFrozenAssignmentReceiver(
        FrozenAssignmentAssemblerConfig config = {});

    void EnqueueFrame(dspgcs::TransportFrame frame);
    DSPGCSFrozenAssignmentProcessStats ProcessPending(
        const std::string& currentFleetSessionId,
        const DroneManager& droneManager,
        FrozenAssignmentStore& store,
        const AckSender& sendAck,
        Clock::time_point now = Clock::now());

    std::size_t GetQueuedFrameCount() const;
    std::size_t GetDroppedFrameCount() const;

    static bool IsFrozenAssignmentMessageType(dspgcs::MessageType type);

private:
    bool EmitAck(
        const FrozenAssignmentTransactionResult& result,
        bool accepted,
        dspgcs::FrozenAssignmentErrorCode errorCode,
        const AckSender& sendAck) const;

    static constexpr std::size_t MaxQueuedFrames = 4096;

    mutable std::mutex mutex_;
    std::deque<dspgcs::TransportFrame> pendingFrames_;
    std::size_t droppedFrameCount_ = 0;
    std::string activeFleetSessionId_;
    FrozenAssignmentTransactionAssembler assembler_;
};
