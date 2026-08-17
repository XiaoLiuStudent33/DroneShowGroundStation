#include "FrozenAssignmentTransactionAssembler.h"

#include <algorithm>
#include <limits>
#include <utility>

FrozenAssignmentTransactionAssembler::FrozenAssignmentTransactionAssembler(
    FrozenAssignmentAssemblerConfig config)
    : config_(std::move(config))
{
}

FrozenAssignmentTransactionResult
FrozenAssignmentTransactionAssembler::Begin(
    const dspgcs::FrozenAssignmentBeginDTO& begin,
    const std::string& currentFleetSessionId,
    const Clock::time_point now)
{
    std::string validationError;
    if (!dspgcs::ValidateFrozenAssignmentBegin(begin, validationError))
    {
        return Failure(begin.transactionId,
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            std::move(validationError));
    }
    if (begin.identity.fleetSessionId != currentFleetSessionId)
    {
        return Failure(begin.transactionId,
            dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch,
            "FleetSessionID does not match the current Console session.");
    }
    if (begin.matchedCount == 0)
    {
        return Failure(begin.transactionId,
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            "A Frozen Assignment must contain at least one matched mapping.");
    }
    if (begin.totalPayloadBytes > config_.maxTransactionBytes ||
        begin.chunkCount > config_.maxChunkCount)
    {
        return Failure(begin.transactionId,
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            "Transaction exceeds Console staging safety bounds.");
    }

    const auto existing = transactions_.find(begin.transactionId);
    if (existing != transactions_.end())
    {
        if (!BeginMetadataEqual(existing->second.begin, begin))
        {
            auto result = Failure(begin.transactionId,
                dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
                "TransactionID was reused with different Begin metadata.");
            result.identity = existing->second.begin.identity;
            result.digest = existing->second.begin.canonicalPayloadDigest;
            result.matchedCount = existing->second.begin.matchedCount;
            return result;
        }
        existing->second.lastActivity = now;
        FrozenAssignmentTransactionResult result;
        result.succeeded = true;
        result.idempotent = true;
        result.transactionId = begin.transactionId;
        result.identity = begin.identity;
        result.digest = begin.canonicalPayloadDigest;
        result.matchedCount = begin.matchedCount;
        return result;
    }

    if (transactions_.size() >= config_.maxStagingTransactions ||
        begin.totalPayloadBytes >
            config_.maxAggregateStagingBytes - aggregateReservedBytes_)
    {
        return Failure(begin.transactionId,
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            "Console staging resource ceiling was reached.");
    }

    StagingTransaction transaction;
    transaction.begin = begin;
    transaction.createdAt = now;
    transaction.lastActivity = now;
    transaction.chunks.reserve(
        std::min<std::size_t>(begin.chunkCount, 4096));
    aggregateReservedBytes_ +=
        static_cast<std::size_t>(begin.totalPayloadBytes);
    transactions_.emplace(begin.transactionId, std::move(transaction));

    FrozenAssignmentTransactionResult result;
    result.succeeded = true;
    result.transactionId = begin.transactionId;
    result.identity = begin.identity;
    result.digest = begin.canonicalPayloadDigest;
    result.matchedCount = begin.matchedCount;
    return result;
}

FrozenAssignmentTransactionResult
FrozenAssignmentTransactionAssembler::AddChunk(
    const dspgcs::FrozenAssignmentChunkDTO& chunk,
    const Clock::time_point now)
{
    const auto item = transactions_.find(chunk.transactionId);
    if (item == transactions_.end())
    {
        return Failure(chunk.transactionId,
            dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
            "Chunk has no active Begin transaction.");
    }

    StagingTransaction& transaction = item->second;
    auto FailureAndRemove = [this, item, &transaction, &chunk](
        const dspgcs::FrozenAssignmentErrorCode code,
        const char* message)
    {
        auto result = Failure(chunk.transactionId, code, message);
        result.identity = transaction.begin.identity;
        result.digest = transaction.begin.canonicalPayloadDigest;
        result.matchedCount = transaction.begin.matchedCount;
        RemoveTransaction(item);
        return result;
    };

    if (chunk.chunkCount != transaction.begin.chunkCount ||
        chunk.chunkIndex >= transaction.begin.chunkCount ||
        chunk.offset > transaction.begin.totalPayloadBytes ||
        chunk.chunkPayloadBytes.size() >
            transaction.begin.totalPayloadBytes - chunk.offset)
    {
        return FailureAndRemove(
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            "Chunk metadata exceeds its Begin transaction bounds.");
    }

    const auto existing = transaction.chunks.find(chunk.chunkIndex);
    if (existing != transaction.chunks.end())
    {
        if (existing->second.offset != chunk.offset ||
            existing->second.bytes != chunk.chunkPayloadBytes)
        {
            return FailureAndRemove(
                dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
                "Duplicate ChunkIndex contained conflicting bytes or offset.");
        }
        transaction.lastActivity = now;
        FrozenAssignmentTransactionResult result;
        result.succeeded = true;
        result.idempotent = true;
        result.transactionId = chunk.transactionId;
        result.identity = transaction.begin.identity;
        result.digest = transaction.begin.canonicalPayloadDigest;
        result.matchedCount = transaction.begin.matchedCount;
        return result;
    }

    if (chunk.chunkPayloadBytes.size() >
            config_.maxAggregateStagingBytes - aggregateReceivedBytes_ ||
        chunk.chunkPayloadBytes.size() >
            config_.maxTransactionBytes - transaction.receivedBytes)
    {
        return FailureAndRemove(
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            "Chunk bytes exceed Console staging safety bounds.");
    }

    ChunkRecord record;
    record.offset = chunk.offset;
    record.bytes = chunk.chunkPayloadBytes;
    transaction.receivedBytes += record.bytes.size();
    aggregateReceivedBytes_ += record.bytes.size();
    transaction.chunks.emplace(chunk.chunkIndex, std::move(record));
    transaction.lastActivity = now;

    FrozenAssignmentTransactionResult result;
    result.succeeded = true;
    result.transactionId = chunk.transactionId;
    result.identity = transaction.begin.identity;
    result.digest = transaction.begin.canonicalPayloadDigest;
    result.matchedCount = transaction.begin.matchedCount;
    return result;
}

FrozenAssignmentTransactionResult
FrozenAssignmentTransactionAssembler::Commit(
    const dspgcs::FrozenAssignmentCommitDTO& commit,
    const std::string& currentFleetSessionId,
    const DroneResolver& resolveDrone,
    const Clock::time_point now)
{
    const auto item = transactions_.find(commit.transactionId);
    if (item == transactions_.end())
    {
        return Failure(commit.transactionId,
            dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
            "Commit has no active Begin transaction.");
    }

    StagingTransaction& transaction = item->second;
    transaction.lastActivity = now;
    auto FinishFailure = [this, item, &transaction, &commit](
        const dspgcs::FrozenAssignmentErrorCode code,
        std::string message)
    {
        auto result = Failure(commit.transactionId, code, std::move(message));
        result.identity = transaction.begin.identity;
        result.digest = transaction.begin.canonicalPayloadDigest;
        result.matchedCount = transaction.begin.matchedCount;
        RemoveTransaction(item);
        return result;
    };

    if (currentFleetSessionId != transaction.begin.identity.fleetSessionId)
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch,
            "FleetSessionID changed before Commit.");
    }
    if (!IdentityEqual(commit.identity, transaction.begin.identity))
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            "Commit identity does not match Begin identity.");
    }
    if (commit.canonicalPayloadDigest !=
        transaction.begin.canonicalPayloadDigest)
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::HashMismatch,
            "Commit digest does not match Begin digest.");
    }
    if (transaction.chunks.size() != transaction.begin.chunkCount)
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
            "Commit arrived before all ChunkIndex values were received.");
    }

    std::vector<const ChunkRecord*> orderedChunks;
    orderedChunks.reserve(transaction.begin.chunkCount);
    for (std::uint32_t chunkIndex = 0;
         chunkIndex < transaction.begin.chunkCount;
         ++chunkIndex)
    {
        const auto chunk = transaction.chunks.find(chunkIndex);
        if (chunk == transaction.chunks.end())
        {
            return FinishFailure(
                dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
                "ChunkIndex coverage is incomplete.");
        }
        orderedChunks.push_back(&chunk->second);
    }
    std::sort(orderedChunks.begin(), orderedChunks.end(),
        [](const ChunkRecord* left, const ChunkRecord* right)
        {
            return left->offset < right->offset;
        });

    std::uint64_t expectedOffset = 0;
    for (const ChunkRecord* chunk : orderedChunks)
    {
        if (chunk->offset != expectedOffset)
        {
            return FinishFailure(
                chunk->offset < expectedOffset
                    ? dspgcs::FrozenAssignmentErrorCode::MalformedAssignment
                    : dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
                chunk->offset < expectedOffset
                    ? "Chunk byte ranges overlap."
                    : "Chunk byte ranges contain a gap.");
        }
        expectedOffset += chunk->bytes.size();
    }
    if (expectedOffset != transaction.begin.totalPayloadBytes)
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
            "Chunk byte ranges do not cover TotalPayloadBytes.");
    }

    std::vector<std::uint8_t> canonicalPayload;
    canonicalPayload.reserve(
        static_cast<std::size_t>(transaction.begin.totalPayloadBytes));
    for (const ChunkRecord* chunk : orderedChunks)
    {
        canonicalPayload.insert(
            canonicalPayload.end(), chunk->bytes.begin(), chunk->bytes.end());
    }

    dspgcs::FrozenAssignmentDigest calculatedDigest{};
    std::string validationError;
    if (!dspgcs::ComputeFrozenAssignmentSha256(
            canonicalPayload, calculatedDigest, validationError))
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::InternalError,
            std::move(validationError));
    }
    if (calculatedDigest != transaction.begin.canonicalPayloadDigest ||
        calculatedDigest != commit.canonicalPayloadDigest)
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::HashMismatch,
            "Canonical payload SHA-256 does not match Begin and Commit.");
    }

    dspgcs::FrozenAssignmentPayloadDTO payload;
    if (!dspgcs::DecodeCanonicalFrozenAssignmentPayload(
            canonicalPayload, payload, validationError))
    {
        const dspgcs::FrozenAssignmentErrorCode decodeErrorCode =
            ClassifyDecodeError(validationError);
        return FinishFailure(
            decodeErrorCode,
            std::move(validationError));
    }
    if (payload.matchedCount == 0 ||
        !PayloadMatchesBegin(payload, transaction.begin))
    {
        return FinishFailure(
            dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
            "Canonical payload metadata does not match Begin metadata.");
    }
    for (const auto& mapping : payload.mappings)
    {
        if (!resolveDrone || !resolveDrone(mapping.droneId))
        {
            return FinishFailure(
                dspgcs::FrozenAssignmentErrorCode::UnknownDroneID,
                "A matched DroneID does not resolve in the current Fleet session.");
        }
    }

    FrozenAssignmentTransactionResult result;
    result.succeeded = true;
    result.transactionId = commit.transactionId;
    result.identity = transaction.begin.identity;
    result.digest = calculatedDigest;
    result.matchedCount = payload.matchedCount;
    result.candidateSnapshot = MakeSnapshot(
        std::move(payload), calculatedDigest);
    RemoveTransaction(item);
    return result;
}

bool FrozenAssignmentTransactionAssembler::Abort(
    const std::string& transactionId)
{
    const auto item = transactions_.find(transactionId);
    if (item == transactions_.end())
    {
        return false;
    }
    RemoveTransaction(item);
    return true;
}

std::size_t FrozenAssignmentTransactionAssembler::PruneExpiredTransactions(
    const Clock::time_point now)
{
    std::size_t removed = 0;
    for (auto item = transactions_.begin(); item != transactions_.end();)
    {
        if (now - item->second.lastActivity >= config_.transactionTimeout)
        {
            auto expired = item++;
            RemoveTransaction(expired);
            ++removed;
        }
        else
        {
            ++item;
        }
    }
    return removed;
}

void FrozenAssignmentTransactionAssembler::Reset()
{
    transactions_.clear();
    aggregateReservedBytes_ = 0;
    aggregateReceivedBytes_ = 0;
}

std::size_t
FrozenAssignmentTransactionAssembler::GetStagingTransactionCount() const
{
    return transactions_.size();
}

std::size_t FrozenAssignmentTransactionAssembler::GetAggregateStagingBytes() const
{
    return aggregateReceivedBytes_;
}

bool FrozenAssignmentTransactionAssembler::TryGetTransactionContext(
    const std::string& transactionId,
    dspgcs::FrozenAssignmentIdentityDTO& identity,
    dspgcs::FrozenAssignmentDigest& digest,
    std::uint32_t& matchedCount) const
{
    const auto item = transactions_.find(transactionId);
    if (item == transactions_.end())
    {
        return false;
    }
    identity = item->second.begin.identity;
    digest = item->second.begin.canonicalPayloadDigest;
    matchedCount = item->second.begin.matchedCount;
    return true;
}

bool FrozenAssignmentTransactionAssembler::BeginMetadataEqual(
    const dspgcs::FrozenAssignmentBeginDTO& left,
    const dspgcs::FrozenAssignmentBeginDTO& right)
{
    return left.payloadSchemaVersion == right.payloadSchemaVersion &&
        left.transactionId == right.transactionId &&
        IdentityEqual(left.identity, right.identity) &&
        left.fleetRevisionAtCapture == right.fleetRevisionAtCapture &&
        left.routeDatasetGeneration == right.routeDatasetGeneration &&
        left.completionMode == right.completionMode &&
        left.freezePolicy == right.freezePolicy &&
        left.routeSlotCount == right.routeSlotCount &&
        left.matchedCount == right.matchedCount &&
        left.spareCount == right.spareCount &&
        left.excludedCount == right.excludedCount &&
        left.vacantCount == right.vacantCount &&
        left.frozenAtUtcMs == right.frozenAtUtcMs &&
        left.totalPayloadBytes == right.totalPayloadBytes &&
        left.chunkCount == right.chunkCount &&
        left.canonicalPayloadDigest == right.canonicalPayloadDigest;
}

bool FrozenAssignmentTransactionAssembler::IdentityEqual(
    const dspgcs::FrozenAssignmentIdentityDTO& left,
    const dspgcs::FrozenAssignmentIdentityDTO& right)
{
    return left.fleetSessionId == right.fleetSessionId &&
        left.assignmentAuthoritySessionId ==
            right.assignmentAuthoritySessionId &&
        left.assignmentRevision == right.assignmentRevision;
}

dspgcs::FrozenAssignmentErrorCode
FrozenAssignmentTransactionAssembler::ClassifyDecodeError(
    const std::string& error)
{
    if (error.find("Duplicate DroneID") != std::string::npos)
        return dspgcs::FrozenAssignmentErrorCode::DuplicateDroneID;
    if (error.find("Duplicate RouteSlotIndex") != std::string::npos)
        return dspgcs::FrozenAssignmentErrorCode::DuplicateRouteSlot;
    if (error.find("outside the route domain") != std::string::npos)
        return dspgcs::FrozenAssignmentErrorCode::RouteSlotOutOfRange;
    return dspgcs::FrozenAssignmentErrorCode::MalformedAssignment;
}

bool FrozenAssignmentTransactionAssembler::PayloadMatchesBegin(
    const dspgcs::FrozenAssignmentPayloadDTO& payload,
    const dspgcs::FrozenAssignmentBeginDTO& begin)
{
    return payload.payloadSchemaVersion == begin.payloadSchemaVersion &&
        IdentityEqual(payload.identity, begin.identity) &&
        payload.fleetRevisionAtCapture == begin.fleetRevisionAtCapture &&
        payload.routeDatasetGeneration == begin.routeDatasetGeneration &&
        payload.completionMode == begin.completionMode &&
        payload.freezePolicy == begin.freezePolicy &&
        payload.routeSlotCount == begin.routeSlotCount &&
        payload.matchedCount == begin.matchedCount &&
        payload.spareCount == begin.spareCount &&
        payload.excludedCount == begin.excludedCount &&
        payload.vacantCount == begin.vacantCount &&
        payload.frozenAtUtcMs == begin.frozenAtUtcMs;
}

FrozenAssignmentAcceptedSnapshot
FrozenAssignmentTransactionAssembler::MakeSnapshot(
    dspgcs::FrozenAssignmentPayloadDTO payload,
    const dspgcs::FrozenAssignmentDigest& digest)
{
    FrozenAssignmentAcceptedSnapshot snapshot;
    snapshot.identity = std::move(payload.identity);
    snapshot.fleetRevisionAtCapture = payload.fleetRevisionAtCapture;
    snapshot.routeDatasetGeneration = payload.routeDatasetGeneration;
    snapshot.completionMode = payload.completionMode;
    snapshot.freezePolicy = payload.freezePolicy;
    snapshot.routeSlotCount = payload.routeSlotCount;
    snapshot.matchedCount = payload.matchedCount;
    snapshot.spareCount = payload.spareCount;
    snapshot.excludedCount = payload.excludedCount;
    snapshot.vacantCount = payload.vacantCount;
    snapshot.frozenAtUtcMs = payload.frozenAtUtcMs;
    snapshot.canonicalPayloadDigest = digest;
    snapshot.matchedMappings = std::move(payload.mappings);
    return snapshot;
}

FrozenAssignmentTransactionResult
FrozenAssignmentTransactionAssembler::Failure(
    const std::string& transactionId,
    const dspgcs::FrozenAssignmentErrorCode errorCode,
    std::string diagnostic) const
{
    FrozenAssignmentTransactionResult result;
    result.errorCode = errorCode;
    result.diagnostic = std::move(diagnostic);
    result.transactionId = transactionId;
    return result;
}

void FrozenAssignmentTransactionAssembler::RemoveTransaction(
    const TransactionMap::iterator item)
{
    aggregateReservedBytes_ -=
        static_cast<std::size_t>(item->second.begin.totalPayloadBytes);
    aggregateReceivedBytes_ -= item->second.receivedBytes;
    transactions_.erase(item);
}
