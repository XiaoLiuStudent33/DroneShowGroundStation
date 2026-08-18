#pragma once

#include "ConsoleArtifactConsumer.h"
#include "DSPGCSUploadJobProtocol.h"
#include "FrozenAssignmentStore.h"
#include "RouteArtifactReadLease.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ConsoleUploadJobAuthority final
{
public:
    static constexpr std::size_t MaxIdempotencyEntries = 1024;
    static constexpr std::size_t RoutePayloadBudgetBytes = 900;

    ConsoleUploadJobAuthority(
        std::string workspaceRoot,
        ConsoleArtifactConsumer* artifactConsumer,
        FrozenAssignmentStore* frozenAssignmentStore);

    dspgcs::UploadJobCommandAckDTO HandleCommand(
        const std::string& clientSessionId,
        const dspgcs::UploadJobCommandDTO& command,
        const std::vector<std::uint8_t>& canonicalCommandPayload);

    void Tick();

    dspgcs::UploadJobSnapshotDTO BuildSnapshot(
        const dspgcs::UploadJobSnapshotRequestDTO& request);
    bool BuildRoutePage(
        const dspgcs::UploadJobSnapshotRequestDTO& request,
        dspgcs::UploadJobRouteStatePageDTO& page,
        dspgcs::UploadJobErrorCode& errorCode);
    std::vector<dspgcs::UploadJobRouteStateDeltaDTO> DrainRouteDeltas();

    /** Narrow future RouteUploadManager seam; it owns the measurements. */
    bool ApplyExecutorRouteUpdates(
        const std::string& uploadJobId,
        const std::vector<dspgcs::UploadRouteStateDTO>& updates,
        std::uint64_t throughputBytesPerSecond,
        std::uint32_t throughputWindowMs,
        std::uint64_t elapsedMs,
        std::string& error);
    bool TransitionJobForExecutor(
        const std::string& uploadJobId,
        dspgcs::UploadJobState newState,
        std::string& error);
    bool DestroyFailedJob(const std::string& uploadJobId);

    const dspgcs::UploadJobSnapshotDTO* FindCurrentSnapshotForTest(
        const std::string& uploadJobId);
    bool InstallSyntheticReadyJobForTest(
        const std::string& uploadJobId,
        const dspgcs::UploadArtifactIdentityDTO& identity,
        std::vector<dspgcs::UploadRouteStateDTO> routes,
        std::uint32_t vacantCount,
        std::uint32_t spareCount,
        std::uint32_t excludedCount,
        std::uint32_t maxConcurrentUploads,
        std::string& error);
    std::size_t GetJobCount() const { return jobs_.size(); }
    std::size_t GetIdempotencyEntryCount() const { return idempotency_.size(); }
    std::size_t GetFrozenPageRouteCountForTest(const std::string& uploadJobId) const;

private:
    struct Job
    {
        dspgcs::UploadJobSnapshotDTO snapshot;
        std::vector<dspgcs::UploadRouteStateDTO> routes;
        std::vector<dspgcs::UploadRouteStateDTO> pageSnapshot;
        std::uint64_t pageSnapshotRevision = 0;
        std::uint64_t snapshotTokenRevision = 0;
        std::string pageSnapshotToken;
        std::uint64_t nextDeltaSequence = 0;
        std::uint64_t validationGeneration = 0;
        std::shared_ptr<const ValidatedArtifactSnapshot> validatedArtifact;
        std::unique_ptr<dronegcs::route_artifact::ArtifactReadLease> artifactLease;
    };

    struct IdempotencyRecord
    {
        std::vector<std::uint8_t> canonicalPayload;
        dspgcs::UploadJobCommandAckDTO acknowledgement;
    };

    static std::string MakeCanonicalId();
    static bool SameBinding(
        const dronegcs::route_artifact::AssignmentBinding& binding,
        const dspgcs::UploadArtifactIdentityDTO& identity);
    static bool SameFrozenBinding(
        const FrozenAssignmentAcceptedSnapshot& frozen,
        const dspgcs::UploadArtifactIdentityDTO& identity);
    static ExpectedArtifactIdentity ToExpectedIdentity(
        const dspgcs::UploadArtifactIdentityDTO& identity);
    static dspgcs::UploadArtifactIdentityDTO ToWireIdentity(
        const ValidatedArtifactSnapshot& snapshot);
    static std::int64_t UtcNowMs();

    dspgcs::UploadJobCommandAckDTO Reject(
        const dspgcs::UploadJobCommandDTO& command,
        dspgcs::UploadJobErrorCode code,
        const char* diagnostic,
        std::uint64_t revision = 0,
        std::uint32_t effectiveConcurrency = 0) const;
    dspgcs::UploadJobCommandAckDTO Accept(
        const dspgcs::UploadJobCommandDTO& command,
        const Job& job) const;
    void RememberCommand(
        const std::string& key,
        std::vector<std::uint8_t> canonicalPayload,
        const dspgcs::UploadJobCommandAckDTO& acknowledgement);
    void CompleteValidation(
        Job& job,
        const ConsoleArtifactValidationResult& result);
    void FailJob(Job& job, dspgcs::UploadJobErrorCode code, const char* diagnostic);
    void RecalculateAggregate(Job& job);
    void QueueRouteDeltas(Job& job, const std::vector<dspgcs::UploadRouteStateDTO>& updates);
    bool AcquireArtifactLease(Job& job, ConsoleArtifactDiagnostic& diagnostic);
    Job* FindJob(const std::string& uploadJobId);

    std::string workspaceRoot_;
    ConsoleArtifactConsumer* artifactConsumer_ = nullptr;
    FrozenAssignmentStore* frozenAssignmentStore_ = nullptr;
    std::shared_ptr<const ValidatedArtifactSnapshot> latestValidatedArtifact_;
    std::unordered_map<std::string, Job> jobs_;
    std::unordered_map<std::uint64_t, std::string> validationJobs_;
    std::unordered_map<std::string, IdempotencyRecord> idempotency_;
    std::deque<std::string> idempotencyOrder_;
    std::vector<dspgcs::UploadJobRouteStateDeltaDTO> pendingDeltas_;
};
