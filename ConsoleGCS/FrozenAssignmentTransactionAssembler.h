#pragma once

#include "FrozenAssignmentStore.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct FrozenAssignmentAssemblerConfig
{
    std::size_t maxStagingTransactions = 32;
    std::size_t maxTransactionBytes =
        dspgcs::MaxFrozenAssignmentPayloadBytes;
    std::size_t maxAggregateStagingBytes =
        2 * dspgcs::MaxFrozenAssignmentPayloadBytes;
    std::uint32_t maxChunkCount = dspgcs::MaxFrozenAssignmentChunks;
    std::chrono::milliseconds transactionTimeout{ 30000 };
};

struct FrozenAssignmentTransactionResult
{
    bool succeeded = false;
    bool idempotent = false;
    dspgcs::FrozenAssignmentErrorCode errorCode =
        dspgcs::FrozenAssignmentErrorCode::None;
    std::string diagnostic;
    std::string transactionId;
    dspgcs::FrozenAssignmentIdentityDTO identity;
    dspgcs::FrozenAssignmentDigest digest{};
    std::uint32_t matchedCount = 0;
    std::optional<FrozenAssignmentAcceptedSnapshot> candidateSnapshot;
};

class FrozenAssignmentTransactionAssembler
{
public:
    using Clock = std::chrono::steady_clock;
    using DroneResolver = std::function<bool(const std::string&)>;

    explicit FrozenAssignmentTransactionAssembler(
        FrozenAssignmentAssemblerConfig config = {});

    FrozenAssignmentTransactionResult Begin(
        const dspgcs::FrozenAssignmentBeginDTO& begin,
        const std::string& currentFleetSessionId,
        Clock::time_point now);
    FrozenAssignmentTransactionResult AddChunk(
        const dspgcs::FrozenAssignmentChunkDTO& chunk,
        Clock::time_point now);
    FrozenAssignmentTransactionResult Commit(
        const dspgcs::FrozenAssignmentCommitDTO& commit,
        const std::string& currentFleetSessionId,
        const DroneResolver& resolveDrone,
        Clock::time_point now);
    bool Abort(const std::string& transactionId);
    std::size_t PruneExpiredTransactions(Clock::time_point now);
    void Reset();

    std::size_t GetStagingTransactionCount() const;
    std::size_t GetAggregateStagingBytes() const;
    bool TryGetTransactionContext(
        const std::string& transactionId,
        dspgcs::FrozenAssignmentIdentityDTO& identity,
        dspgcs::FrozenAssignmentDigest& digest,
        std::uint32_t& matchedCount) const;

private:
    struct ChunkRecord
    {
        std::uint64_t offset = 0;
        std::vector<std::uint8_t> bytes;
    };

    struct StagingTransaction
    {
        dspgcs::FrozenAssignmentBeginDTO begin;
        Clock::time_point createdAt;
        Clock::time_point lastActivity;
        std::unordered_map<std::uint32_t, ChunkRecord> chunks;
        std::size_t receivedBytes = 0;
    };

    using TransactionMap =
        std::unordered_map<std::string, StagingTransaction>;

    static bool BeginMetadataEqual(
        const dspgcs::FrozenAssignmentBeginDTO& left,
        const dspgcs::FrozenAssignmentBeginDTO& right);
    static bool IdentityEqual(
        const dspgcs::FrozenAssignmentIdentityDTO& left,
        const dspgcs::FrozenAssignmentIdentityDTO& right);
    static dspgcs::FrozenAssignmentErrorCode ClassifyDecodeError(
        const std::string& error);
    static bool PayloadMatchesBegin(
        const dspgcs::FrozenAssignmentPayloadDTO& payload,
        const dspgcs::FrozenAssignmentBeginDTO& begin);
    static FrozenAssignmentAcceptedSnapshot MakeSnapshot(
        dspgcs::FrozenAssignmentPayloadDTO payload,
        const dspgcs::FrozenAssignmentDigest& digest);

    FrozenAssignmentTransactionResult Failure(
        const std::string& transactionId,
        dspgcs::FrozenAssignmentErrorCode errorCode,
        std::string diagnostic) const;
    void RemoveTransaction(TransactionMap::iterator item);

    FrozenAssignmentAssemblerConfig config_;
    TransactionMap transactions_;
    std::size_t aggregateReservedBytes_ = 0;
    std::size_t aggregateReceivedBytes_ = 0;
};
