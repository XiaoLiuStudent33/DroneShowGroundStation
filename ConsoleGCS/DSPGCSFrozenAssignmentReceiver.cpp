#include "DSPGCSFrozenAssignmentReceiver.h"

#include "DroneManager.h"

#include <algorithm>
#include <utility>
#include <vector>

DSPGCSFrozenAssignmentReceiver::DSPGCSFrozenAssignmentReceiver(
    FrozenAssignmentAssemblerConfig config)
    : assembler_(std::move(config))
{
}

void DSPGCSFrozenAssignmentReceiver::EnqueueFrame(
    dspgcs::TransportFrame frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pendingFrames_.size() >= MaxQueuedFrames)
    {
        ++droppedFrameCount_;
        return;
    }
    pendingFrames_.push_back(std::move(frame));
}

DSPGCSFrozenAssignmentProcessStats
DSPGCSFrozenAssignmentReceiver::ProcessPending(
    const std::string& currentFleetSessionId,
    const DroneManager& droneManager,
    FrozenAssignmentStore& store,
    const AckSender& sendAck,
    const Clock::time_point now)
{
    DSPGCSFrozenAssignmentProcessStats stats;
    if (activeFleetSessionId_ != currentFleetSessionId)
    {
        activeFleetSessionId_ = currentFleetSessionId;
        assembler_.Reset();
    }
    store.SynchronizeFleetSession(currentFleetSessionId);
    stats.expiredTransactions = assembler_.PruneExpiredTransactions(now);

    std::deque<dspgcs::TransportFrame> frames;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames.swap(pendingFrames_);
    }

    for (dspgcs::TransportFrame& frame : frames)
    {
        ++stats.framesProcessed;
        dspgcs::MessageEnvelope envelope;
        std::vector<std::uint8_t> payloadBytes;
        if (!dspgcs::DecodeEnvelope(frame.bytes, envelope, payloadBytes) ||
            !IsFrozenAssignmentMessageType(envelope.messageType))
        {
            ++stats.transactionsRejected;
            continue;
        }

        if (envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Begin)
        {
            dspgcs::FrozenAssignmentBeginDTO begin;
            std::string error;
            if (!dspgcs::DecodeFrozenAssignmentBegin(
                    payloadBytes, begin, error))
            {
                ++stats.transactionsRejected;
                continue;
            }

            const FrozenAssignmentStoreResult preflight =
                store.EvaluateCandidate(
                    begin.identity, begin.canonicalPayloadDigest);
            if (!preflight.accepted)
            {
                FrozenAssignmentTransactionResult result;
                result.transactionId = begin.transactionId;
                result.identity = begin.identity;
                result.digest = begin.canonicalPayloadDigest;
                result.matchedCount = begin.matchedCount;
                result.diagnostic = "Assignment revision or authority epoch was rejected.";
                if (EmitAck(result, false, preflight.errorCode, sendAck))
                    ++stats.acknowledgementsSent;
                ++stats.transactionsRejected;
                continue;
            }

            auto result = assembler_.Begin(
                begin, currentFleetSessionId, now);
            if (!result.succeeded)
            {
                if (EmitAck(result, false, result.errorCode, sendAck))
                    ++stats.acknowledgementsSent;
                ++stats.transactionsRejected;
            }
            continue;
        }

        if (envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Chunk)
        {
            dspgcs::FrozenAssignmentChunkDTO chunk;
            std::string error;
            if (!dspgcs::DecodeFrozenAssignmentChunk(
                    payloadBytes, chunk, error))
            {
                ++stats.transactionsRejected;
                continue;
            }

            dspgcs::FrozenAssignmentIdentityDTO contextIdentity;
            dspgcs::FrozenAssignmentDigest contextDigest{};
            std::uint32_t contextMatchedCount = 0;
            const bool hasContext = assembler_.TryGetTransactionContext(
                chunk.transactionId,
                contextIdentity,
                contextDigest,
                contextMatchedCount);
            auto result = assembler_.AddChunk(chunk, now);
            if (!result.succeeded)
            {
                if (hasContext && result.identity.fleetSessionId.empty())
                {
                    result.identity = std::move(contextIdentity);
                    result.digest = contextDigest;
                    result.matchedCount = contextMatchedCount;
                }
                if (hasContext &&
                    EmitAck(result, false, result.errorCode, sendAck))
                    ++stats.acknowledgementsSent;
                ++stats.transactionsRejected;
            }
            continue;
        }

        if (envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Commit)
        {
            dspgcs::FrozenAssignmentCommitDTO commit;
            std::string error;
            if (!dspgcs::DecodeFrozenAssignmentCommit(
                    payloadBytes, commit, error))
            {
                ++stats.transactionsRejected;
                continue;
            }

            auto result = assembler_.Commit(
                commit,
                currentFleetSessionId,
                [&droneManager](const std::string& droneId)
                {
                    return droneManager.FindDrone(droneId) != nullptr;
                },
                now);
            if (!result.succeeded || !result.candidateSnapshot)
            {
                if (!result.identity.fleetSessionId.empty() &&
                    EmitAck(result, false, result.errorCode, sendAck))
                    ++stats.acknowledgementsSent;
                ++stats.transactionsRejected;
                continue;
            }

            const FrozenAssignmentStoreResult storeResult =
                store.Accept(std::move(*result.candidateSnapshot));
            if (!storeResult.accepted)
            {
                if (EmitAck(
                        result, false, storeResult.errorCode, sendAck))
                    ++stats.acknowledgementsSent;
                ++stats.transactionsRejected;
                continue;
            }

            const auto accepted = store.GetAcceptedSnapshot(
                currentFleetSessionId);
            if (accepted)
            {
                result.digest = accepted->canonicalPayloadDigest;
                result.matchedCount = accepted->matchedCount;
            }
            if (EmitAck(
                    result,
                    true,
                    dspgcs::FrozenAssignmentErrorCode::None,
                    sendAck))
                ++stats.acknowledgementsSent;
            ++stats.transactionsAccepted;
            continue;
        }

        if (envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Abort)
        {
            dspgcs::FrozenAssignmentAbortDTO abort;
            std::string error;
            if (!dspgcs::DecodeFrozenAssignmentAbort(
                    payloadBytes, abort, error))
            {
                ++stats.transactionsRejected;
                continue;
            }
            assembler_.Abort(abort.transactionId);
            continue;
        }

        // FrozenAssignmentAck is Console outbound in this phase. An inbound
        // ACK is consumed by this dispatcher and never falls into Command.
    }
    return stats;
}

std::size_t DSPGCSFrozenAssignmentReceiver::GetQueuedFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingFrames_.size();
}

std::size_t DSPGCSFrozenAssignmentReceiver::GetDroppedFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedFrameCount_;
}

bool DSPGCSFrozenAssignmentReceiver::IsFrozenAssignmentMessageType(
    const dspgcs::MessageType type)
{
    return type == dspgcs::FrozenAssignmentMessageTypes::Begin ||
        type == dspgcs::FrozenAssignmentMessageTypes::Chunk ||
        type == dspgcs::FrozenAssignmentMessageTypes::Commit ||
        type == dspgcs::FrozenAssignmentMessageTypes::Ack ||
        type == dspgcs::FrozenAssignmentMessageTypes::Abort;
}

bool DSPGCSFrozenAssignmentReceiver::EmitAck(
    const FrozenAssignmentTransactionResult& result,
    const bool accepted,
    const dspgcs::FrozenAssignmentErrorCode errorCode,
    const AckSender& sendAck) const
{
    if (!sendAck || result.transactionId.empty() ||
        result.identity.fleetSessionId.empty() ||
        result.identity.assignmentAuthoritySessionId.empty() ||
        result.identity.assignmentRevision == 0)
    {
        return false;
    }

    dspgcs::FrozenAssignmentAckDTO ack;
    ack.transactionId = result.transactionId;
    ack.identity = result.identity;
    ack.result = accepted
        ? dspgcs::FrozenAssignmentAckResult::Accepted
        : dspgcs::FrozenAssignmentAckResult::Rejected;
    ack.errorCode = errorCode;
    if (accepted)
    {
        ack.acceptedDigest = result.digest;
        ack.acceptedMatchedCount = result.matchedCount;
    }
    ack.diagnostic = result.diagnostic.substr(
        0, dspgcs::MaxFrozenAssignmentDiagnosticBytes);
    return sendAck(ack);
}
