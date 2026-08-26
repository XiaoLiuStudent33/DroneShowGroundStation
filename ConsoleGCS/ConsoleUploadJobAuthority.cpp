#include "ConsoleUploadJobAuthority.h"
#include "ConsoleUI.h"
#include "RouteMissionCodec.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace
{
    dronegcs::route_artifact::Digest256 ToArtifactDigest(
        const dspgcs::UploadJobDigest& source)
    {
        dronegcs::route_artifact::Digest256 result;
        std::copy(source.begin(), source.end(), result.Bytes.begin());
        result.IsSet = true;
        return result;
    }

    dspgcs::UploadJobDigest ToWireDigest(
        const dronegcs::route_artifact::Digest256& source)
    {
        dspgcs::UploadJobDigest result{};
        if (source.IsSet)
            std::copy(source.Bytes.begin(), source.Bytes.end(), result.begin());
        return result;
    }

    bool SameStartBinding(
        const dspgcs::UploadJobSnapshotDTO& snapshot,
        const dspgcs::UploadJobCommandDTO& command)
    {
        return dspgcs::UploadArtifactBindingEqual(
                snapshot.artifactBinding, command.artifactBinding) &&
            dspgcs::UploadFrozenAssignmentBindingEqual(
                snapshot.frozenAssignmentBinding,
                command.frozenAssignmentBinding) &&
            dspgcs::UploadRuntimeConfigBindingEqual(
                snapshot.frozenMavFtpConfig.identity,
                command.runtimeConfigBinding);
    }

    bool IsTerminalJobState(const dspgcs::UploadJobState state)
    {
        return state == dspgcs::UploadJobState::Completed ||
            state == dspgcs::UploadJobState::Failed ||
            state == dspgcs::UploadJobState::Cancelled;
    }

    std::uint64_t ElapsedSince(
        const std::chrono::steady_clock::time_point start)
    {
        if (start == std::chrono::steady_clock::time_point{}) return 0;
        const auto now = std::chrono::steady_clock::now();
        if (now <= start) return 0;
        return static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::milliseconds>(now - start).count());
    }
}

ConsoleUploadJobAuthority::ConsoleUploadJobAuthority(
    std::string workspaceRoot,
    ConsoleArtifactConsumerV2* artifactConsumer,
    FrozenAssignmentStore* frozenAssignmentStore,
    const ConsoleMavFtpBootConfig mavFtpBootConfig)
    : workspaceRoot_(std::move(workspaceRoot))
    , artifactConsumer_(artifactConsumer)
    , frozenAssignmentStore_(frozenAssignmentStore)
{
    runtimeConfig_.authoritySessionId = MakeCanonicalId();
    runtimeConfig_.configRevision = 1;
    runtimeConfig_.bootDefaultGlobalWriteCap =
        mavFtpBootConfig.globalWriteOutstandingCap;
    runtimeConfig_.runtimeAcceptedGlobalWriteCap =
        mavFtpBootConfig.globalWriteOutstandingCap;
    runtimeConfig_.writeWindow = mavFtpBootConfig.writeWindow;
    runtimeConfig_.writePayload = dspgcs::MavFtpRuntimeConfigWirePayloadBytes;
    runtimeConfig_.segmentItems = mavFtpBootConfig.segmentItems;
    runtimeConfig_.requestTimeoutMs = mavFtpBootConfig.requestTimeoutMs;
    runtimeConfig_.terminateTimeoutMs = mavFtpBootConfig.terminateTimeoutMs;
    runtimeConfig_.retryCount = mavFtpBootConfig.retryCount;
    runtimeConfig_.activationState =
        dspgcs::MavFtpRuntimeConfigActivationState::BootDefaults;
    std::string digestError;
    if (!dspgcs::ComputeMavFtpRuntimeConfigDigest(
            runtimeConfig_, runtimeConfig_.configDigest, digestError))
        throw std::runtime_error(digestError);
}

std::uint32_t ConsoleUploadJobAuthority::ComputeEffectiveMaxConcurrentTargets(
    const std::uint32_t requested,
    const std::size_t matchedTargetCount)
{
    if (requested == 0 || matchedTargetCount == 0)
        return 0;
    return std::min<std::uint32_t>({
        requested,
        static_cast<std::uint32_t>(std::min<std::size_t>(
            matchedTargetCount,
            std::numeric_limits<std::uint32_t>::max())) });
}

std::int64_t ConsoleUploadJobAuthority::UtcNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string ConsoleUploadJobAuthority::MakeCanonicalId()
{
    static thread_local std::mt19937_64 generator(std::random_device{}());
    std::ostringstream stream;
    stream << std::hex << std::setfill('0')
        << std::setw(16) << generator() << std::setw(16) << generator();
    return stream.str();
}

dspgcs::UploadJobFrozenMavFtpConfigDTO
ConsoleUploadJobAuthority::FreezeMavFtpConfig(
    const dspgcs::MavFtpRuntimeConfigSnapshotDTO& runtimeConfig,
    const std::uint32_t requestedMaxConcurrentTargets,
    const std::uint32_t effectiveMaxConcurrentTargets)
{
    dspgcs::UploadJobFrozenMavFtpConfigDTO result;
    result.identity.authoritySessionId = runtimeConfig.authoritySessionId;
    result.identity.configRevision = runtimeConfig.configRevision;
    result.identity.configDigest = runtimeConfig.configDigest;
    result.globalWriteOutstandingCap =
        runtimeConfig.runtimeAcceptedGlobalWriteCap;
    result.writeWindow = runtimeConfig.writeWindow;
    result.writePayload = runtimeConfig.writePayload;
    result.segmentItems = runtimeConfig.segmentItems;
    result.requestTimeoutMs = runtimeConfig.requestTimeoutMs;
    result.terminateTimeoutMs = runtimeConfig.terminateTimeoutMs;
    result.retryCount = runtimeConfig.retryCount;
    result.requestedMaxConcurrentTargets = requestedMaxConcurrentTargets;
    result.effectiveMaxConcurrentTargets = effectiveMaxConcurrentTargets;
    result.requiredGlobalWriteOutstandingCap =
        static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            static_cast<std::uint64_t>(effectiveMaxConcurrentTargets) *
                runtimeConfig.writeWindow,
            dspgcs::MavFtpRuntimeConfigMaximumGlobalCap));
    result.effectiveGlobalWriteOutstandingCap = (std::min)(
        result.globalWriteOutstandingCap,
        result.requiredGlobalWriteOutstandingCap);
    return result;
}

dspgcs::MavFtpRuntimeConfigSnapshotDTO
ConsoleUploadJobAuthority::BuildRuntimeConfigSnapshot(
    const std::string& clientRequestId) const
{
    auto result = runtimeConfig_;
    result.clientRequestId = clientRequestId;
    return result;
}

dspgcs::MavFtpRuntimeConfigAckDTO
ConsoleUploadJobAuthority::HandleRuntimeConfigRequest(
    const std::string& clientSessionId,
    const dspgcs::MavFtpRuntimeConfigRequestDTO& request,
    const std::vector<std::uint8_t>& canonicalRequestPayload)
{
    const std::string key = clientSessionId + ":" + request.clientRequestId;
    const auto existing = runtimeConfigIdempotency_.find(key);
    if (existing != runtimeConfigIdempotency_.end())
    {
        auto replay = existing->second.acknowledgement;
        if (existing->second.canonicalPayload == canonicalRequestPayload)
            replay.status = dspgcs::MavFtpRuntimeConfigAckStatus::DuplicateReplayed;
        else
        {
            replay.status = dspgcs::MavFtpRuntimeConfigAckStatus::Rejected;
            replay.resultCode =
                dspgcs::MavFtpRuntimeConfigErrorCode::IdempotencyConflict;
            replay.safeDiagnostic =
                "ClientRequestID was reused with a different payload.";
        }
        return replay;
    }

    dspgcs::MavFtpRuntimeConfigAckDTO acknowledgement;
    acknowledgement.clientRequestId = request.clientRequestId;
    acknowledgement.authoritySessionId = runtimeConfig_.authoritySessionId;
    acknowledgement.authoritativeRevision = runtimeConfig_.configRevision;
    acknowledgement.configDigest = runtimeConfig_.configDigest;
    acknowledgement.status = dspgcs::MavFtpRuntimeConfigAckStatus::Rejected;

    std::string validationError;
    if (!dspgcs::ValidateMavFtpRuntimeConfigRequest(request, validationError))
    {
        acknowledgement.resultCode =
            dspgcs::MavFtpRuntimeConfigErrorCode::InvalidRequest;
        acknowledgement.safeDiagnostic = validationError;
    }
    else if (request.operation == dspgcs::MavFtpRuntimeConfigOperation::Get)
    {
        acknowledgement.status = dspgcs::MavFtpRuntimeConfigAckStatus::Accepted;
        acknowledgement.resultCode = dspgcs::MavFtpRuntimeConfigErrorCode::None;
    }
    else if (request.authoritySessionId != runtimeConfig_.authoritySessionId)
    {
        acknowledgement.resultCode =
            dspgcs::MavFtpRuntimeConfigErrorCode::AuthoritySessionMismatch;
        acknowledgement.safeDiagnostic = "Runtime Config Authority session is stale.";
    }
    else if (request.expectedConfigRevision != runtimeConfig_.configRevision)
    {
        acknowledgement.resultCode =
            dspgcs::MavFtpRuntimeConfigErrorCode::StaleRevision;
        acknowledgement.safeDiagnostic = "ExpectedConfigRevision is stale.";
    }
    else
    {
        auto candidate = runtimeConfig_;
        candidate.clientRequestId.clear();
        candidate.runtimeAcceptedGlobalWriteCap =
            request.candidateGlobalWriteOutstandingCap;
        candidate.activationState =
            dspgcs::MavFtpRuntimeConfigActivationState::RuntimeAccepted;
        ++candidate.configRevision;
        if (!dspgcs::ComputeMavFtpRuntimeConfigDigest(
                candidate, candidate.configDigest, validationError))
        {
            acknowledgement.resultCode =
                dspgcs::MavFtpRuntimeConfigErrorCode::InternalError;
            acknowledgement.safeDiagnostic = "Runtime Config digest failed.";
        }
        else
        {
            runtimeConfig_ = candidate;
            acknowledgement.status =
                dspgcs::MavFtpRuntimeConfigAckStatus::Accepted;
            acknowledgement.resultCode =
                dspgcs::MavFtpRuntimeConfigErrorCode::None;
            acknowledgement.authoritativeRevision = runtimeConfig_.configRevision;
            acknowledgement.configDigest = runtimeConfig_.configDigest;
            ConsoleUI::AddLog(
                "RuntimeConfig",
                "MAVFTP Runtime Config applied AuthoritySessionID=" +
                    runtimeConfig_.authoritySessionId + " ConfigRevision=" +
                    std::to_string(runtimeConfig_.configRevision) +
                    " GlobalWriteOutstandingCap=" +
                    std::to_string(runtimeConfig_.runtimeAcceptedGlobalWriteCap));
        }
    }

    while (runtimeConfigIdempotency_.size() >= MaxIdempotencyEntries &&
        !runtimeConfigIdempotencyOrder_.empty())
    {
        runtimeConfigIdempotency_.erase(runtimeConfigIdempotencyOrder_.front());
        runtimeConfigIdempotencyOrder_.pop_front();
    }
    runtimeConfigIdempotency_.emplace(key, RuntimeConfigIdempotencyRecord{
        canonicalRequestPayload, acknowledgement });
    runtimeConfigIdempotencyOrder_.push_back(key);
    return acknowledgement;
}

bool ConsoleUploadJobAuthority::SameFrozenBinding(
    const FrozenAssignmentAcceptedSnapshot& frozen,
    const dspgcs::UploadFrozenAssignmentBindingDTO& binding)
{
    return frozen.identity.fleetSessionId == binding.fleetSessionId &&
        frozen.identity.assignmentAuthoritySessionId ==
            binding.assignmentAuthoritySessionId &&
        frozen.identity.assignmentRevision == binding.assignmentRevision &&
        frozen.canonicalPayloadDigest == binding.assignmentDigest;
}

ExpectedRouteArtifactV2Identity ConsoleUploadJobAuthority::ToExpectedIdentity(
    const dspgcs::UploadArtifactBindingDTO& binding)
{
    ExpectedRouteArtifactV2Identity result;
    result.artifactSetId = binding.artifactSetId;
    result.artifactSetDigest = ToArtifactDigest(binding.artifactSetDigest);
    return result;
}

dspgcs::UploadArtifactBindingDTO
ConsoleUploadJobAuthority::ToWireArtifactBinding(
    const AcceptedRouteArtifactV2& snapshot)
{
    dspgcs::UploadArtifactBindingDTO result;
    result.artifactSetId = snapshot.artifactSetId;
    result.artifactSetDigest = ToWireDigest(snapshot.artifactSetDigest);
    return result;
}

dspgcs::UploadStableSourceDTO ConsoleUploadJobAuthority::ToWireStableSource(
    const AcceptedRouteArtifactV2StableSource& stableSource)
{
    dspgcs::UploadStableSourceDTO result;
    result.sourceDatasetDigest = ToWireDigest(stableSource.sourceDatasetDigest);
    result.normalizedShowOriginLatitudeE7 =
        stableSource.normalizedShowOriginLatitudeE7;
    result.normalizedShowOriginLongitudeE7 =
        stableSource.normalizedShowOriginLongitudeE7;
    result.routeSlotCount = stableSource.routeSlotCount > 0
        ? static_cast<std::uint32_t>(stableSource.routeSlotCount) : 0;
    return result;
}

dspgcs::UploadJobCommandAckDTO ConsoleUploadJobAuthority::Reject(
    const dspgcs::UploadJobCommandDTO& command,
    const dspgcs::UploadJobErrorCode code,
    const char* diagnostic,
    const std::uint64_t revision,
    const std::uint32_t effectiveConcurrency) const
{
    dspgcs::UploadJobCommandAckDTO result;
    result.clientCommandId = command.clientCommandId;
    result.uploadJobId = command.uploadJobId;
    result.commandType = command.commandType;
    result.status = dspgcs::UploadJobCommandAckStatus::Rejected;
    result.resultCode = code;
    result.authoritativeJobRevision = revision;
    result.effectiveMaxConcurrentUploads = effectiveConcurrency;
    result.safeDiagnostic = diagnostic ? diagnostic : "";
    return result;
}

dspgcs::UploadJobCommandAckDTO ConsoleUploadJobAuthority::Accept(
    const dspgcs::UploadJobCommandDTO& command,
    const std::uint64_t revision,
    const std::uint32_t effectiveConcurrency) const
{
    dspgcs::UploadJobCommandAckDTO result;
    result.clientCommandId = command.clientCommandId;
    result.uploadJobId = command.uploadJobId;
    result.commandType = command.commandType;
    result.status = dspgcs::UploadJobCommandAckStatus::Accepted;
    result.resultCode = dspgcs::UploadJobErrorCode::None;
    result.authoritativeJobRevision = revision;
    result.effectiveMaxConcurrentUploads = effectiveConcurrency;
    return result;
}

void ConsoleUploadJobAuthority::RememberCommand(
    const std::string& key,
    std::vector<std::uint8_t> canonicalPayload,
    const dspgcs::UploadJobCommandAckDTO& acknowledgement)
{
    while (idempotency_.size() >= MaxIdempotencyEntries &&
        !idempotencyOrder_.empty())
    {
        idempotency_.erase(idempotencyOrder_.front());
        idempotencyOrder_.pop_front();
    }
    idempotencyOrder_.push_back(key);
    idempotency_[key] = { std::move(canonicalPayload), acknowledgement };
}

ConsoleUploadJobAuthority::Job* ConsoleUploadJobAuthority::FindJob(
    const std::string& uploadJobId)
{
    const auto item = jobs_.find(uploadJobId);
    return item == jobs_.end() ? nullptr : &item->second;
}

dspgcs::UploadJobCommandAckDTO ConsoleUploadJobAuthority::HandleCommand(
    const std::string& clientSessionId,
    const dspgcs::UploadJobCommandDTO& command,
    const std::vector<std::uint8_t>& canonicalCommandPayload)
{
    const auto commandStartedAt = std::chrono::steady_clock::now();
    const std::string idempotencyKey =
        clientSessionId + ":" + command.clientCommandId;
    const auto previous = idempotency_.find(idempotencyKey);
    if (previous != idempotency_.end())
    {
        if (previous->second.canonicalPayload == canonicalCommandPayload)
            return previous->second.acknowledgement;
        return Reject(command, dspgcs::UploadJobErrorCode::IdempotencyConflict,
            "ClientCommandID was already used with a different payload.");
    }

    dspgcs::UploadJobCommandAckDTO acknowledgement;
    Job* job = FindJob(command.uploadJobId);
    if (command.commandType == dspgcs::UploadJobCommandType::Start)
    {
        const auto pendingItem = pendingStarts_.find(command.uploadJobId);
        if (job)
        {
            acknowledgement = SameStartBinding(job->snapshot, command)
                ? Accept(command, job->snapshot.jobRevision,
                    job->snapshot.effectiveMaxConcurrentUploads)
                : Reject(command, dspgcs::UploadJobErrorCode::JobIdCollision,
                    "UploadJobID is already bound to different authorities.",
                    job->snapshot.jobRevision,
                    job->snapshot.effectiveMaxConcurrentUploads);
        }
        else if (pendingItem != pendingStarts_.end())
        {
            acknowledgement = SameStartBinding(pendingItem->second.snapshot, command)
                ? Accept(command, pendingItem->second.snapshot.jobRevision,
                    pendingItem->second.snapshot.effectiveMaxConcurrentUploads)
                : Reject(command, dspgcs::UploadJobErrorCode::JobIdCollision,
                    "UploadJobID is already pending with different authorities.");
        }
        else if (command.requestedMaxConcurrentUploads == 0 ||
            command.requestedMaxConcurrentUploads > 50)
        {
            acknowledgement = Reject(command,
                dspgcs::UploadJobErrorCode::InvalidMaxConcurrency,
                "Requested MaxConcurrentUploads must be in range 1..50.");
        }
        else if (command.runtimeConfigBinding.authoritySessionId !=
            runtimeConfig_.authoritySessionId)
        {
            acknowledgement = Reject(command,
                dspgcs::UploadJobErrorCode::RuntimeConfigIdentityMismatch,
                "Start Runtime Config AuthoritySessionID is stale.");
        }
        else if (command.runtimeConfigBinding.configRevision !=
            runtimeConfig_.configRevision)
        {
            acknowledgement = Reject(command,
                dspgcs::UploadJobErrorCode::RuntimeConfigStale,
                "Start Runtime Config revision is stale.");
        }
        else if (command.runtimeConfigBinding.configDigest !=
            runtimeConfig_.configDigest)
        {
            acknowledgement = Reject(command,
                dspgcs::UploadJobErrorCode::RuntimeConfigIdentityMismatch,
                "Start Runtime Config digest does not match Console Authority.");
        }
        else if (!artifactConsumer_ || !frozenAssignmentStore_)
        {
            acknowledgement = Reject(command,
                dspgcs::UploadJobErrorCode::ArtifactNotAccepted,
                "Upload Job authorities are unavailable.");
        }
        else if (!pendingStarts_.empty())
        {
            acknowledgement = Reject(command,
                dspgcs::UploadJobErrorCode::InvalidJobState,
                "Another Artifact validation is already pending.");
        }
        else
        {
            const std::string currentFleetSessionId =
                frozenAssignmentStore_->GetCurrentFleetSessionID();
            if (currentFleetSessionId !=
                command.frozenAssignmentBinding.fleetSessionId)
            {
                acknowledgement = Reject(command,
                    dspgcs::UploadJobErrorCode::FleetSessionStale,
                    "Start FleetSessionID is not the current Console Fleet session.");
            }
            else
            {
                const auto frozen = frozenAssignmentStore_->GetAcceptedSnapshot(
                    command.frozenAssignmentBinding.fleetSessionId);
                if (!frozen)
                {
                    acknowledgement = Reject(command,
                        dspgcs::UploadJobErrorCode::FrozenAssignmentMissing,
                        "No Console-accepted Frozen Assignment exists for this Fleet session.");
                }
                else if (!SameFrozenBinding(
                    *frozen, command.frozenAssignmentBinding))
                {
                    acknowledgement = Reject(command,
                        dspgcs::UploadJobErrorCode::FrozenAssignmentIdentityMismatch,
                        "Start does not match the current accepted Frozen Assignment.");
                }
                else
                {
                    PendingStart pending;
                    pending.snapshot.uploadJobId = command.uploadJobId;
                    pending.snapshot.artifactBinding = command.artifactBinding;
                    pending.snapshot.frozenAssignmentBinding =
                        command.frozenAssignmentBinding;
                    pending.snapshot.artifactGateState =
                        dspgcs::UploadArtifactGateState::Validating;
                    pending.snapshot.jobState = dspgcs::UploadJobState::Validating;
                    pending.snapshot.jobRevision = 1;
                    pending.snapshot.snapshotSequence = 1;
                    pending.snapshot.snapshotToken = MakeCanonicalId();
                    pending.snapshot.updatedAtUtcMs = UtcNowMs();
                    pending.snapshot.effectiveMaxConcurrentUploads =
                        ComputeEffectiveMaxConcurrentTargets(
                            command.requestedMaxConcurrentUploads,
                            frozen->matchedMappings.size());
                    pending.snapshot.frozenMavFtpConfig = FreezeMavFtpConfig(
                        runtimeConfig_,
                        command.requestedMaxConcurrentUploads,
                        pending.snapshot.effectiveMaxConcurrentUploads);
                    pending.createdAt = std::chrono::steady_clock::now();
                    pending.idempotencyKey = idempotencyKey;
                    pending.command = command;
                    pending.expectedArtifact =
                        ToExpectedIdentity(command.artifactBinding);
                    pending.frozenSnapshot = frozen;
                    ConsoleArtifactV2ValidationScope validationScope;
                    validationScope.fullArtifactValidation = false;
                    validationScope.frozenAssignmentRevision =
                        frozen->identity.assignmentRevision;
                    std::copy(frozen->canonicalPayloadDigest.begin(),
                        frozen->canonicalPayloadDigest.end(),
                        validationScope.frozenAssignmentDigest.begin());
                    validationScope.matchedRoutes.reserve(
                        frozen->matchedMappings.size());
                    for (const auto& mapping : frozen->matchedMappings)
                    {
                        validationScope.matchedRoutes.push_back(
                            { mapping.droneId, mapping.routeSlotIndex });
                    }
                    pending.authorityLookupDurationMs = ElapsedSince(commandStartedAt);
                    pending.validationRequestedAt = std::chrono::steady_clock::now();
                    pending.validationGeneration =
                        artifactConsumer_->RequestValidation(
                            pending.expectedArtifact, std::move(validationScope));
                    ConsoleUI::AddLog(
                        "[UploadJobStage] Stage=JobAccepted UploadJobID=" +
                        command.uploadJobId + " ArtifactSetID=" +
                        pending.expectedArtifact.artifactSetId +
                        " FrozenRevision=" +
                        std::to_string(frozen->identity.assignmentRevision) +
                        " MatchedTargets=" +
                        std::to_string(frozen->matchedMappings.size()) +
                        " RouteSlots=" + std::to_string(frozen->routeSlotCount) +
                        " Vacant=" + std::to_string(frozen->vacantCount) +
                        " DurationMs=" +
                        std::to_string(pending.authorityLookupDurationMs) +
                        " ValidationGeneration=" +
                        std::to_string(pending.validationGeneration) +
                        " WaitOrRetry=no");
                    validationStarts_[pending.validationGeneration] =
                        command.uploadJobId;
                    const std::uint64_t revision = pending.snapshot.jobRevision;
                    const std::uint32_t concurrency =
                        pending.snapshot.effectiveMaxConcurrentUploads;
                    pendingStarts_.emplace(command.uploadJobId, std::move(pending));
                    startOutcomes_.erase(command.uploadJobId);
                    acknowledgement = Accept(command, revision, concurrency);
                }
            }
        }
    }
    else if (!job)
    {
        acknowledgement = Reject(command, dspgcs::UploadJobErrorCode::UnknownJob,
            pendingStarts_.find(command.uploadJobId) != pendingStarts_.end()
                ? "Upload Job Start is still pending validation."
                : "Upload Job does not exist.");
    }
    else if (command.expectedJobRevision != job->snapshot.jobRevision)
    {
        acknowledgement = Reject(command,
            dspgcs::UploadJobErrorCode::StaleJobRevision,
            "ExpectedJobRevision is stale.",
            job->snapshot.jobRevision,
            job->snapshot.effectiveMaxConcurrentUploads);
    }
    else
    {
        bool accepted = true;
        switch (command.commandType)
        {
        case dspgcs::UploadJobCommandType::Pause:
            if (job->snapshot.jobState != dspgcs::UploadJobState::Ready &&
                job->snapshot.jobState != dspgcs::UploadJobState::Running)
                accepted = false;
            else job->snapshot.jobState = dspgcs::UploadJobState::Paused;
            break;
        case dspgcs::UploadJobCommandType::Resume:
            if (job->snapshot.jobState != dspgcs::UploadJobState::Paused)
                accepted = false;
            else job->snapshot.jobState = dspgcs::UploadJobState::Running;
            break;
        case dspgcs::UploadJobCommandType::Cancel:
            if (job->snapshot.jobState == dspgcs::UploadJobState::Completed ||
                job->snapshot.jobState == dspgcs::UploadJobState::Cancelled)
                accepted = false;
            else
            {
                job->snapshot.jobState = dspgcs::UploadJobState::Cancelling;
            }
            break;
        case dspgcs::UploadJobCommandType::RetryFailed:
            if (job->snapshot.jobState != dspgcs::UploadJobState::Failed ||
                job->snapshot.failedCount == 0 || !job->artifactLease)
                accepted = false;
            else
            {
                for (auto& target : job->targets)
                {
                    auto& route = target.state;
                    if (route.state == dspgcs::UploadRouteState::Failed)
                    {
                        route.state = dspgcs::UploadRouteState::Waiting;
                        route.bytesCompleted = 0;
                        route.errorCode = dspgcs::RouteUploadErrorCode::None;
                        route.safeFailureSummary.clear();
                        route.retryable = false;
                    }
                }
                job->snapshot.jobState = dspgcs::UploadJobState::Ready;
                job->snapshot.firstFailure = {};
                RecalculateAggregate(*job);
            }
            break;
        case dspgcs::UploadJobCommandType::SetMaxConcurrentUploads:
            // E.1 freezes concurrency into Start. A later UI edit is for the
            // next Job and cannot mutate an in-flight Job transaction.
            accepted = false;
            break;
        default:
            accepted = false;
            break;
        }
        if (!accepted)
        {
            acknowledgement = Reject(command,
                command.commandType == dspgcs::UploadJobCommandType::RetryFailed
                    ? dspgcs::UploadJobErrorCode::NoFailedRoutes
                    : dspgcs::UploadJobErrorCode::InvalidJobState,
                "Command is not legal in the current Job state.",
                job->snapshot.jobRevision,
                job->snapshot.effectiveMaxConcurrentUploads);
        }
        else
        {
            ++job->snapshot.jobRevision;
            job->snapshot.updatedAtUtcMs = UtcNowMs();
            acknowledgement = Accept(command, job->snapshot.jobRevision,
                job->snapshot.effectiveMaxConcurrentUploads);
        }
    }

    RememberCommand(idempotencyKey, canonicalCommandPayload, acknowledgement);
    return acknowledgement;
}

void ConsoleUploadJobAuthority::FailPendingStart(
    const std::string& uploadJobId,
    const dspgcs::UploadJobErrorCode code,
    const char* diagnostic,
    const PendingStart* pending)
{
    startOutcomes_[uploadJobId] = {
        code, diagnostic ? diagnostic : "Upload Job Start failed." };
    if (pending)
    {
        const auto record = idempotency_.find(pending->idempotencyKey);
        if (record != idempotency_.end())
        {
            record->second.acknowledgement = Reject(
                pending->command, code, diagnostic);
        }
    }
}

bool ConsoleUploadJobAuthority::AcquireArtifactLease(
    const PendingStart& pending,
    const RouteArtifactV2CurrentDirectoryIdentity& currentDirectoryIdentity,
    std::unique_ptr<dronegcs::route_artifact::ArtifactReadLease>& lease,
    ConsoleArtifactV2Diagnostic& diagnostic)
{
    if (!artifactConsumer_->IsCurrentIdentityStill(
        pending.expectedArtifact, currentDirectoryIdentity, diagnostic))
        return false;

    auto candidate =
        std::make_unique<dronegcs::route_artifact::ArtifactReadLease>();
    bool recovered = false;
    std::string error;
    const auto result = candidate->TryAcquire(
        std::filesystem::u8path(workspaceRoot_).wstring(), recovered, error);
    if (result !=
        dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
    {
        diagnostic.errorCode = ConsoleArtifactV2ErrorCode::IoFailure;
        diagnostic.safeDetail = result ==
            dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Busy
            ? "Artifact Current promotion gate is busy."
            : "Artifact Current lease acquisition failed.";
        return false;
    }
    if (!artifactConsumer_->IsCurrentIdentityStill(
        pending.expectedArtifact, currentDirectoryIdentity, diagnostic))
        return false;
    lease = std::move(candidate);
    return true;
}

void ConsoleUploadJobAuthority::CompletePendingStart(
    const std::string& uploadJobId,
    PendingStart pending,
    const ConsoleArtifactV2ValidationResult& result)
{
    const auto materializationStartedAt = std::chrono::steady_clock::now();
    std::uint64_t routeResolutionDurationMs = 0;
    std::uint64_t leaseDurationMs = 0;
    auto fail = [this, &uploadJobId, &pending](
        const dspgcs::UploadJobErrorCode code, const char* message)
    {
        FailPendingStart(uploadJobId, code, message, &pending);
    };

    if (!result.success || !result.snapshot)
    {
        const auto code = result.diagnostic.errorCode ==
                ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch
            ? dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch
            : result.diagnostic.errorCode ==
                ConsoleArtifactV2ErrorCode::FileChangedDuringValidation
                ? dspgcs::UploadJobErrorCode::ArtifactCurrentChanged
                : dspgcs::UploadJobErrorCode::ArtifactNotAccepted;
        fail(code, "Artifact v2 validation did not produce the requested accepted snapshot.");
        return;
    }

    const auto artifactBinding = ToWireArtifactBinding(*result.snapshot);
    if (!dspgcs::UploadArtifactBindingEqual(
        artifactBinding, pending.snapshot.artifactBinding))
    {
        fail(dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch,
            "Validated Artifact identity differs from Start.");
        return;
    }
    if (!pending.frozenSnapshot || !SameFrozenBinding(
        *pending.frozenSnapshot, pending.snapshot.frozenAssignmentBinding))
    {
        fail(dspgcs::UploadJobErrorCode::FrozenAssignmentIdentityMismatch,
            "Captured Frozen Assignment identity differs from Start.");
        return;
    }

    const auto artifactStable = ToWireStableSource(
        result.snapshot->stableSource);
    const auto& frozen = *pending.frozenSnapshot;
    if (artifactStable.sourceDatasetDigest != frozen.sourceDatasetDigest)
    {
        fail(dspgcs::UploadJobErrorCode::StableSourceDatasetMismatch,
            "Artifact and Frozen Assignment source dataset digests differ.");
        return;
    }
    if (artifactStable.normalizedShowOriginLatitudeE7 !=
            frozen.normalizedShowOriginLatitudeE7 ||
        artifactStable.normalizedShowOriginLongitudeE7 !=
            frozen.normalizedShowOriginLongitudeE7)
    {
        fail(dspgcs::UploadJobErrorCode::StableSourceOriginMismatch,
            "Artifact and Frozen Assignment normalized origins differ.");
        return;
    }
    if (artifactStable.routeSlotCount != frozen.routeSlotCount)
    {
        fail(dspgcs::UploadJobErrorCode::RouteDomainMismatch,
            "Artifact and Frozen Assignment route domains differ.");
        return;
    }
    if (frozen.matchedCount != frozen.matchedMappings.size() ||
        static_cast<std::uint64_t>(frozen.matchedCount) + frozen.vacantCount !=
            frozen.routeSlotCount)
    {
        fail(dspgcs::UploadJobErrorCode::InvalidMapping,
            "Frozen Assignment mapping counts are inconsistent.");
        return;
    }

    std::unordered_set<std::string> droneIds;
    std::unordered_set<std::int32_t> routeSlots;
    droneIds.reserve(frozen.matchedMappings.size());
    routeSlots.reserve(frozen.matchedMappings.size());
    std::vector<Target> targets;
    targets.reserve(frozen.matchedMappings.size());
    std::size_t missionPayloadBytes = 0;
    dronegcs::route_mission::Error missionSizeError;
    if (result.snapshot->frameCount <= 0 ||
        !dronegcs::route_mission::CheckedMissionPayloadSize(
            static_cast<std::uint32_t>(result.snapshot->frameCount),
            missionPayloadBytes,
            missionSizeError))
    {
        fail(dspgcs::UploadJobErrorCode::ArtifactNotAccepted,
            "Artifact FrameCount cannot be represented by the Mission codec.");
        return;
    }
    for (const auto& mapping : frozen.matchedMappings)
    {
        if (mapping.droneId.empty() || mapping.routeSlotIndex < 0 ||
            static_cast<std::uint64_t>(mapping.routeSlotIndex) >=
                frozen.routeSlotCount ||
            !droneIds.insert(mapping.droneId).second ||
            !routeSlots.insert(mapping.routeSlotIndex).second)
        {
            fail(dspgcs::UploadJobErrorCode::InvalidMapping,
                "Frozen Assignment contains an invalid or duplicate mapping.");
            return;
        }
        const std::size_t routeIndex =
            static_cast<std::size_t>(mapping.routeSlotIndex);
        if (routeIndex >= result.snapshot->routes.size() ||
            result.snapshot->routes[routeIndex].routeSlotIndex !=
                mapping.routeSlotIndex ||
            result.snapshot->routes[routeIndex].fileByteSize < 0 ||
            !result.snapshot->routes[routeIndex].validationConfirmed)
        {
            fail(dspgcs::UploadJobErrorCode::RouteRecordMissing,
                "A Frozen Assignment RouteSlot has no validated Artifact record.");
            return;
        }
        const auto& record = result.snapshot->routes[routeIndex];
        Target target;
        target.metadata.droneId = mapping.droneId;
        target.metadata.routeSlotIndex = mapping.routeSlotIndex;
        target.metadata.relativePath = record.relativePath;
        target.metadata.fileByteSize = record.fileByteSize;
        target.metadata.missionItemCount =
            static_cast<std::uint32_t>(result.snapshot->frameCount);
        target.metadata.missionPayloadByteSize =
            static_cast<std::uint64_t>(missionPayloadBytes);
        target.metadata.routeArtifactDigest = record.routeArtifactDigest;
        target.state.routeSlotIndex = mapping.routeSlotIndex;
        target.state.droneId = mapping.droneId;
        target.state.state = dspgcs::UploadRouteState::Waiting;
        target.state.totalBytes = target.metadata.missionPayloadByteSize;
        target.state.updatedAtUtcMs = UtcNowMs();
        targets.push_back(std::move(target));
    }
    std::sort(targets.begin(), targets.end(),
        [](const Target& left, const Target& right)
        {
            return left.metadata.routeSlotIndex < right.metadata.routeSlotIndex;
        });
    routeResolutionDurationMs = ElapsedSince(materializationStartedAt);

    const std::string currentFleetSessionId =
        frozenAssignmentStore_->GetCurrentFleetSessionID();
    if (currentFleetSessionId !=
        pending.snapshot.frozenAssignmentBinding.fleetSessionId)
    {
        fail(dspgcs::UploadJobErrorCode::FleetSessionStale,
            "Fleet session changed during Artifact validation.");
        return;
    }
    const auto currentFrozen = frozenAssignmentStore_->GetAcceptedSnapshot(
        pending.snapshot.frozenAssignmentBinding.fleetSessionId);
    if (!currentFrozen)
    {
        fail(dspgcs::UploadJobErrorCode::FrozenAssignmentMissing,
            "Frozen Assignment disappeared during Artifact validation.");
        return;
    }
    if (!SameFrozenBinding(
        *currentFrozen, pending.snapshot.frozenAssignmentBinding))
    {
        fail(dspgcs::UploadJobErrorCode::FrozenAssignmentIdentityMismatch,
            "Frozen Assignment changed during Artifact validation.");
        return;
    }

    std::unique_ptr<dronegcs::route_artifact::ArtifactReadLease> lease;
    ConsoleArtifactV2Diagnostic diagnostic;
    const auto leaseStartedAt = std::chrono::steady_clock::now();
    if (!AcquireArtifactLease(
        pending, result.snapshot->currentDirectoryIdentity, lease, diagnostic))
    {
        const auto code = diagnostic.errorCode ==
                ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch
            ? dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch
            : diagnostic.errorCode ==
                ConsoleArtifactV2ErrorCode::FileChangedDuringValidation
                ? dspgcs::UploadJobErrorCode::ArtifactCurrentChanged
                : dspgcs::UploadJobErrorCode::LeaseAcquireFailed;
        fail(code, "Artifact Current changed or its read lease could not be acquired.");
        return;
    }
    leaseDurationMs = ElapsedSince(leaseStartedAt);

    Job job;
    job.snapshot = pending.snapshot;
    job.snapshot.artifactBinding = artifactBinding;
    job.snapshot.stableSource = artifactStable;
    job.snapshot.artifactGateState =
        dspgcs::UploadArtifactGateState::Validated;
    job.snapshot.jobState = dspgcs::UploadJobState::Ready;
    job.snapshot.jobRevision = pending.snapshot.jobRevision + 1;
    job.snapshot.updatedAtUtcMs = UtcNowMs();
    job.snapshot.matchedCount = frozen.matchedCount;
    job.snapshot.vacantCount = frozen.vacantCount;
    job.snapshot.spareCount = frozen.spareCount;
    job.snapshot.excludedCount = frozen.excludedCount;
    job.targets = std::move(targets);
    job.artifactSnapshot = result.snapshot;
    job.frozenSnapshot = pending.frozenSnapshot;
    job.currentDirectoryIdentity = result.snapshot->currentDirectoryIdentity;
    job.artifactLease = std::move(lease);
    job.createdAt = pending.createdAt;
    job.requestedMaxConcurrentUploads =
        pending.command.requestedMaxConcurrentUploads;
    RecalculateAggregate(job);

    const std::uint64_t revision = job.snapshot.jobRevision;
    const std::uint32_t concurrency =
        job.snapshot.effectiveMaxConcurrentUploads;
    const auto insertion = jobs_.emplace(uploadJobId, std::move(job));
    if (!insertion.second)
    {
        fail(dspgcs::UploadJobErrorCode::JobIdCollision,
            "UploadJobID was materialized concurrently.");
        return;
    }
    startOutcomes_.erase(uploadJobId);
    const auto record = idempotency_.find(pending.idempotencyKey);
    if (record != idempotency_.end())
    {
        record->second.acknowledgement =
            Accept(pending.command, revision, concurrency);
    }
    ConsoleUI::AddLog(
        "[UploadJobStage] Stage=TargetCreation UploadJobID=" + uploadJobId +
        " ArtifactSetID=" + pending.expectedArtifact.artifactSetId +
        " FrozenRevision=" + std::to_string(frozen.identity.assignmentRevision) +
        " MatchedTargets=" + std::to_string(frozen.matchedMappings.size()) +
        " RouteSlots=" + std::to_string(frozen.routeSlotCount) +
        " RouteFilesAccessed=0 BytesRead=0 ParsedMissionItems=0 CacheHit=" +
        (result.metrics.cacheHit ? "yes" : "no") +
        " RouteResolutionMs=" + std::to_string(routeResolutionDurationMs) +
        " LeaseMs=" + std::to_string(leaseDurationMs) +
        " DurationMs=" + std::to_string(ElapsedSince(materializationStartedAt)) +
        " AcceptedToReadyMs=" + std::to_string(ElapsedSince(pending.createdAt)) +
        " WaitOrRetry=no");
}

void ConsoleUploadJobAuthority::Tick()
{
    if (!artifactConsumer_) return;
    ConsoleArtifactV2ValidationResult result;
    while (artifactConsumer_->PollCompleted(result))
    {
        const auto generationItem = validationStarts_.find(result.generation);
        if (generationItem == validationStarts_.end()) continue;
        const std::string uploadJobId = generationItem->second;
        validationStarts_.erase(generationItem);
        const auto pendingItem = pendingStarts_.find(uploadJobId);
        if (pendingItem == pendingStarts_.end()) continue;
        PendingStart pending = std::move(pendingItem->second);
        pendingStarts_.erase(pendingItem);
        ConsoleUI::AddLog(
            "[UploadJobStage] Stage=ArtifactValidation UploadJobID=" +
            uploadJobId + " ArtifactSetID=" +
            pending.expectedArtifact.artifactSetId + " FrozenRevision=" +
            std::to_string(pending.frozenSnapshot
                ? pending.frozenSnapshot->identity.assignmentRevision : 0) +
            " DurationMs=" + std::to_string(
                ElapsedSince(pending.validationRequestedAt)) +
            " MetadataMs=" +
            std::to_string(result.metrics.metadataDurationMs) +
            " RouteValidationMs=" +
            std::to_string(result.metrics.routeValidationDurationMs) +
            " RecheckMs=" +
            std::to_string(result.metrics.metadataRecheckDurationMs) +
            " CacheIdentityMs=" +
            std::to_string(result.metrics.cacheIdentityCheckDurationMs) +
            " CacheHit=" + (result.metrics.cacheHit ? "yes" : "no") +
            " ManifestRoutes=" +
            std::to_string(result.metrics.manifestRouteCount) +
            " SelectedRoutes=" +
            std::to_string(result.metrics.selectedRouteCount) +
            " RouteFilesRead=" +
            std::to_string(result.metrics.routeFilesRead) +
            " RouteFilesOpened=" +
            std::to_string(result.metrics.physicalRouteOpenCount) +
            " BytesRead=" + std::to_string(result.metrics.bytesRead) +
            " ParsedMissionItems=" +
            std::to_string(result.metrics.rowsParsed) +
            " ValidationGeneration=" +
            std::to_string(result.generation) + " WaitOrRetry=no Result=" +
            (result.success ? "Succeeded" : "Failed"));
        CompletePendingStart(uploadJobId, std::move(pending), result);
    }
}

void ConsoleUploadJobAuthority::RecalculateAggregate(Job& job)
{
    auto& snapshot = job.snapshot;
    snapshot.waitingCount = snapshot.preparingCount = snapshot.uploadingCount = 0;
    snapshot.retryWaitingCount = snapshot.succeededCount = snapshot.failedCount = 0;
    snapshot.cancelledCount = 0;
    snapshot.totalBytes = snapshot.bytesCompleted = 0;
    snapshot.artifactMissionItemsTotal = 0;
    snapshot.plannedSegmentCountTotal = 0;
    for (const auto& target : job.targets)
    {
        const auto& route = target.state;
        snapshot.totalBytes += route.totalBytes;
        snapshot.bytesCompleted += route.bytesCompleted;
        snapshot.artifactMissionItemsTotal += target.metadata.missionItemCount;
        if (target.metadata.missionItemCount != 0 &&
            snapshot.frozenMavFtpConfig.segmentItems != 0)
        {
            const std::uint64_t wireItems =
                static_cast<std::uint64_t>(target.metadata.missionItemCount) + 1;
            snapshot.plannedSegmentCountTotal +=
                (wireItems + snapshot.frozenMavFtpConfig.segmentItems - 1) /
                snapshot.frozenMavFtpConfig.segmentItems;
        }
        switch (route.state)
        {
        case dspgcs::UploadRouteState::Waiting: ++snapshot.waitingCount; break;
        case dspgcs::UploadRouteState::Preparing: ++snapshot.preparingCount; break;
        case dspgcs::UploadRouteState::Uploading: ++snapshot.uploadingCount; break;
        case dspgcs::UploadRouteState::RetryWaiting: ++snapshot.retryWaitingCount; break;
        case dspgcs::UploadRouteState::Succeeded: ++snapshot.succeededCount; break;
        case dspgcs::UploadRouteState::Failed: ++snapshot.failedCount; break;
        case dspgcs::UploadRouteState::Cancelled: ++snapshot.cancelledCount; break;
        }
    }
    RecalculateFirstFailure(job);
}

void ConsoleUploadJobAuthority::RecalculateFirstFailure(Job& job)
{
    const Target* first = nullptr;
    for (const auto& target : job.targets)
    {
        if (target.state.state != dspgcs::UploadRouteState::Failed) continue;
        if (!first || target.state.routeSlotIndex < first->state.routeSlotIndex ||
            (target.state.routeSlotIndex == first->state.routeSlotIndex &&
             target.state.droneId < first->state.droneId))
        {
            first = &target;
        }
    }
    job.snapshot.firstFailure = {};
    if (!first) return;
    job.snapshot.firstFailure.routeErrorCode = first->state.errorCode;
    job.snapshot.firstFailure.routeSlotIndex = first->state.routeSlotIndex;
    job.snapshot.firstFailure.droneId = first->state.droneId;
    job.snapshot.firstFailure.retryable = first->state.retryable;
    job.snapshot.firstFailure.occurredAtUtcMs = first->state.updatedAtUtcMs;
    job.snapshot.firstFailure.safeSummary =
        first->state.safeFailureSummary.substr(0, 512);
}

dspgcs::UploadJobSnapshotDTO ConsoleUploadJobAuthority::BuildSnapshot(
    const dspgcs::UploadJobSnapshotRequestDTO& request)
{
    Job* job = FindJob(request.uploadJobId);
    if (job)
    {
        if (!IsTerminalJobState(job->snapshot.jobState))
            job->snapshot.elapsedMs = ElapsedSince(job->createdAt);
        job->snapshot.clientRequestId = request.clientRequestId;
        job->snapshot.snapshotToken = MakeCanonicalId();
        ++job->snapshot.snapshotSequence;
        job->pageSnapshot.clear();
        job->pageSnapshot.reserve(job->targets.size());
        for (const auto& target : job->targets)
            job->pageSnapshot.push_back(target.state);
        job->pageSnapshotRevision = job->snapshot.jobRevision;
        job->pageSnapshotToken = job->snapshot.snapshotToken;
        return job->snapshot;
    }
    const auto pendingItem = pendingStarts_.find(request.uploadJobId);
    if (pendingItem != pendingStarts_.end())
    {
        auto& snapshot = pendingItem->second.snapshot;
        snapshot.elapsedMs = ElapsedSince(pendingItem->second.createdAt);
        snapshot.clientRequestId = request.clientRequestId;
        snapshot.snapshotToken = MakeCanonicalId();
        ++snapshot.snapshotSequence;
        return snapshot;
    }
    dspgcs::UploadJobSnapshotDTO error;
    error.clientRequestId = request.clientRequestId;
    error.uploadJobId = request.uploadJobId;
    const auto outcome = startOutcomes_.find(request.uploadJobId);
    if (outcome != startOutcomes_.end())
    {
        error.resultCode = outcome->second.errorCode;
        error.safeDiagnostic = outcome->second.safeDiagnostic;
    }
    else
    {
        error.resultCode = dspgcs::UploadJobErrorCode::UnknownJob;
        error.safeDiagnostic = "Upload Job does not exist in this Console process.";
    }
    return error;
}

bool ConsoleUploadJobAuthority::BuildRoutePage(
    const dspgcs::UploadJobSnapshotRequestDTO& request,
    dspgcs::UploadJobRouteStatePageDTO& page,
    dspgcs::UploadJobErrorCode& errorCode)
{
    errorCode = dspgcs::UploadJobErrorCode::None;
    Job* job = FindJob(request.uploadJobId);
    if (!job)
    {
        errorCode = dspgcs::UploadJobErrorCode::UnknownJob;
        return false;
    }
    if (request.snapshotToken != job->pageSnapshotToken)
    {
        errorCode = dspgcs::UploadJobErrorCode::StaleJobRevision;
        return false;
    }
    if (request.routePageStart >= job->pageSnapshot.size())
    {
        errorCode = dspgcs::UploadJobErrorCode::MalformedMessage;
        return false;
    }
    page.clientRequestId = request.clientRequestId;
    page.uploadJobId = request.uploadJobId;
    page.snapshotToken = request.snapshotToken;
    page.jobRevision = job->pageSnapshotRevision;
    page.totalRouteCount = static_cast<std::uint32_t>(job->pageSnapshot.size());
    page.pageStart = request.routePageStart;
    const std::size_t fixedBytes = 135;
    std::size_t encodedBytes = fixedBytes;
    const std::size_t maximum = std::min<std::size_t>(
        request.maximumRouteEntries,
        job->pageSnapshot.size() - request.routePageStart);
    for (std::size_t offset = 0; offset < maximum; ++offset)
    {
        const auto& route = job->pageSnapshot[request.routePageStart + offset];
        const std::size_t routeBytes = dspgcs::UploadRouteStateEncodedBytes(route);
        if (encodedBytes + routeBytes > RoutePayloadBudgetBytes)
            break;
        encodedBytes += routeBytes;
        page.routes.push_back(route);
    }
    if (page.routes.empty())
    {
        errorCode = dspgcs::UploadJobErrorCode::MalformedMessage;
        return false;
    }
    page.nextPageStart = page.pageStart +
        static_cast<std::uint32_t>(page.routes.size());
    page.hasMore = page.nextPageStart < page.totalRouteCount;
    return true;
}

void ConsoleUploadJobAuthority::QueueRouteDeltas(
    Job& job,
    const std::vector<dspgcs::UploadRouteStateDTO>& updates)
{
    dspgcs::UploadJobRouteStateDeltaDTO batch;
    batch.uploadJobId = job.snapshot.uploadJobId;
    batch.jobRevision = job.snapshot.jobRevision;
    const std::size_t fixedBytes = 58;
    std::size_t encodedBytes = fixedBytes;
    for (const auto& update : updates)
    {
        const std::size_t routeBytes = dspgcs::UploadRouteStateEncodedBytes(update);
        if (!batch.routes.empty() &&
            encodedBytes + routeBytes > RoutePayloadBudgetBytes)
        {
            batch.deltaSequence = ++job.nextDeltaSequence;
            pendingDeltas_.push_back(std::move(batch));
            batch = {};
            batch.uploadJobId = job.snapshot.uploadJobId;
            batch.jobRevision = job.snapshot.jobRevision;
            encodedBytes = fixedBytes;
        }
        encodedBytes += routeBytes;
        batch.routes.push_back(update);
    }
    if (!batch.routes.empty())
    {
        batch.deltaSequence = ++job.nextDeltaSequence;
        pendingDeltas_.push_back(std::move(batch));
    }
    job.snapshot.routeDeltaSequence = job.nextDeltaSequence;
}

bool ConsoleUploadJobAuthority::ApplyExecutorRouteUpdates(
    const std::string& uploadJobId,
    const std::vector<dspgcs::UploadRouteStateDTO>& updates,
    const std::uint64_t throughputBytesPerSecond,
    const std::uint32_t throughputWindowMs,
    const std::uint64_t elapsedMs,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job || !job->artifactLease || updates.empty())
    {
        error = "Executor update requires a leased existing Job.";
        return false;
    }
    std::vector<std::pair<std::size_t, const dspgcs::UploadRouteStateDTO*>>
        resolvedUpdates;
    resolvedUpdates.reserve(updates.size());
    for (const auto& update : updates)
    {
        const auto item = std::lower_bound(job->targets.begin(), job->targets.end(),
            update.routeSlotIndex,
            [](const Target& target, const std::int32_t slot)
            { return target.metadata.routeSlotIndex < slot; });
        const auto stateValue = static_cast<std::uint8_t>(update.state);
        if (item == job->targets.end() ||
            item->metadata.routeSlotIndex != update.routeSlotIndex ||
            update.droneId != item->metadata.droneId ||
            update.totalBytes != item->state.totalBytes ||
            stateValue < static_cast<std::uint8_t>(
                dspgcs::UploadRouteState::Waiting) ||
            stateValue > static_cast<std::uint8_t>(
                dspgcs::UploadRouteState::Cancelled) ||
            update.bytesCompleted > update.totalBytes)
        {
            error = "Executor route update identity is invalid.";
            return false;
        }
        if (update.attemptCount < item->state.attemptCount ||
            update.attemptCount > item->state.attemptCount + 1 ||
            (update.attemptCount == item->state.attemptCount &&
                update.bytesCompleted < item->state.bytesCompleted) ||
            (update.attemptCount > item->state.attemptCount &&
                update.bytesCompleted != 0))
        {
            error = "Executor route update attempt or progress is stale.";
            return false;
        }
        resolvedUpdates.emplace_back(
            static_cast<std::size_t>(item - job->targets.begin()), &update);
    }
    auto adjustStateCount = [](dspgcs::UploadJobSnapshotDTO& snapshot,
        const dspgcs::UploadRouteState state, const bool increment)
    {
        std::uint32_t* count = nullptr;
        switch (state)
        {
        case dspgcs::UploadRouteState::Waiting: count = &snapshot.waitingCount; break;
        case dspgcs::UploadRouteState::Preparing: count = &snapshot.preparingCount; break;
        case dspgcs::UploadRouteState::Uploading: count = &snapshot.uploadingCount; break;
        case dspgcs::UploadRouteState::RetryWaiting: count = &snapshot.retryWaitingCount; break;
        case dspgcs::UploadRouteState::Succeeded: count = &snapshot.succeededCount; break;
        case dspgcs::UploadRouteState::Failed: count = &snapshot.failedCount; break;
        case dspgcs::UploadRouteState::Cancelled: count = &snapshot.cancelledCount; break;
        }
        if (increment) ++(*count); else --(*count);
    };
    bool failureSetChanged = false;
    for (const auto& resolved : resolvedUpdates)
    {
        auto& current = job->targets[resolved.first].state;
        const auto& update = *resolved.second;
        failureSetChanged = failureSetChanged ||
            current.state == dspgcs::UploadRouteState::Failed ||
            update.state == dspgcs::UploadRouteState::Failed;
        adjustStateCount(job->snapshot, current.state, false);
        adjustStateCount(job->snapshot, update.state, true);
        job->snapshot.bytesCompleted -= current.bytesCompleted;
        job->snapshot.bytesCompleted += update.bytesCompleted;
        current = update;
    }
    ++job->snapshot.jobRevision;
    job->snapshot.updatedAtUtcMs = UtcNowMs();
    job->snapshot.throughputBytesPerSecond = throughputBytesPerSecond;
    job->snapshot.throughputWindowMs = throughputWindowMs;
    job->snapshot.elapsedMs = elapsedMs;
    if (failureSetChanged) RecalculateFirstFailure(*job);
    QueueRouteDeltas(*job, updates);
    error.clear();
    return true;
}

bool ConsoleUploadJobAuthority::RefreshExecutorAggregate(
    const std::string& uploadJobId,
    const std::uint64_t throughputBytesPerSecond,
    const std::uint32_t throughputWindowMs,
    const std::uint64_t elapsedMs,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job || !job->artifactLease || IsTerminalJobState(job->snapshot.jobState))
    {
        error = "Executor aggregate refresh requires a live leased Job.";
        return false;
    }
    ++job->snapshot.jobRevision;
    job->snapshot.updatedAtUtcMs = UtcNowMs();
    job->snapshot.throughputBytesPerSecond = throughputBytesPerSecond;
    job->snapshot.throughputWindowMs = throughputWindowMs;
    job->snapshot.elapsedMs = elapsedMs;
    error.clear();
    return true;
}

bool ConsoleUploadJobAuthority::TransitionJobForExecutor(
    const std::string& uploadJobId,
    const dspgcs::UploadJobState newState,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job || !job->artifactLease)
    {
        error = "Executor transition requires a leased existing Job.";
        return false;
    }
    const auto oldState = job->snapshot.jobState;
    const bool legal =
        (oldState == dspgcs::UploadJobState::Ready &&
            newState == dspgcs::UploadJobState::Running) ||
        ((oldState == dspgcs::UploadJobState::Running ||
          oldState == dspgcs::UploadJobState::Paused) &&
            (newState == dspgcs::UploadJobState::Completed ||
             newState == dspgcs::UploadJobState::Failed));
    if (!legal)
    {
        error = "Executor Job transition is invalid.";
        return false;
    }
    job->snapshot.jobState = newState;
    ++job->snapshot.jobRevision;
    job->snapshot.updatedAtUtcMs = UtcNowMs();
    if (newState == dspgcs::UploadJobState::Completed)
        job->artifactLease.reset();
    error.clear();
    return true;
}

std::optional<UploadJobExecutionView>
ConsoleUploadJobAuthority::GetExecutionView(
    const std::string& uploadJobId) const
{
    const auto item = jobs_.find(uploadJobId);
    if (item == jobs_.end()) return std::nullopt;
    const Job& job = item->second;
    if (!job.artifactLease || !job.artifactSnapshot || !job.frozenSnapshot)
        return std::nullopt;

    UploadJobExecutionView view;
    view.uploadJobId = job.snapshot.uploadJobId;
    view.jobRevision = job.snapshot.jobRevision;
    view.createdAt = job.createdAt;
    view.artifactBinding = job.snapshot.artifactBinding;
    view.frozenBinding = job.snapshot.frozenAssignmentBinding;
    view.requestedMaxConcurrentUploads = job.requestedMaxConcurrentUploads;
    view.effectiveMaxConcurrentUploads =
        job.snapshot.effectiveMaxConcurrentUploads;
    view.frozenMavFtpConfig = job.snapshot.frozenMavFtpConfig;
    view.artifactSnapshot = job.artifactSnapshot;
    view.frozenSnapshot = job.frozenSnapshot;
    view.currentDirectoryIdentity = job.currentDirectoryIdentity;
    view.artifactReadLease = job.artifactLease.get();
    view.targets.reserve(job.targets.size());
    for (const Target& target : job.targets)
        view.targets.push_back(target.metadata);
    return view;
}

std::vector<std::string> ConsoleUploadJobAuthority::GetExecutableJobIds() const
{
    std::vector<std::string> result;
    result.reserve(jobs_.size());
    for (const auto& entry : jobs_)
    {
        if (!entry.second.artifactLease) continue;
        const auto state = entry.second.snapshot.jobState;
        if (state == dspgcs::UploadJobState::Ready ||
            state == dspgcs::UploadJobState::Running ||
            state == dspgcs::UploadJobState::Paused ||
            state == dspgcs::UploadJobState::Cancelling)
            result.push_back(entry.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<dspgcs::UploadJobSnapshotDTO>
ConsoleUploadJobAuthority::GetExecutorSnapshot(
    const std::string& uploadJobId) const
{
    const auto item = jobs_.find(uploadJobId);
    if (item == jobs_.end()) return std::nullopt;
    return item->second.snapshot;
}

std::vector<dspgcs::UploadRouteStateDTO>
ConsoleUploadJobAuthority::GetExecutorRouteStates(
    const std::string& uploadJobId) const
{
    std::vector<dspgcs::UploadRouteStateDTO> result;
    const auto item = jobs_.find(uploadJobId);
    if (item == jobs_.end()) return result;
    result.reserve(item->second.targets.size());
    for (const Target& target : item->second.targets)
        result.push_back(target.state);
    return result;
}

bool ConsoleUploadJobAuthority::IsCancellationRequested(
    const std::string& uploadJobId) const
{
    const auto item = jobs_.find(uploadJobId);
    return item != jobs_.end() &&
        item->second.snapshot.jobState == dspgcs::UploadJobState::Cancelling;
}

void ConsoleUploadJobAuthority::FinalizeCancellation(Job& job)
{
    const std::int64_t now = UtcNowMs();
    std::vector<dspgcs::UploadRouteStateDTO> updates;
    updates.reserve(job.targets.size());
    for (Target& target : job.targets)
    {
        if (target.state.state == dspgcs::UploadRouteState::Succeeded ||
            target.state.state == dspgcs::UploadRouteState::Cancelled)
            continue;
        target.state.state = dspgcs::UploadRouteState::Cancelled;
        target.state.errorCode = dspgcs::RouteUploadErrorCode::Cancelled;
        target.state.retryable = false;
        target.state.safeFailureSummary.clear();
        target.state.updatedAtUtcMs = now;
        updates.push_back(target.state);
    }
    job.snapshot.jobState = dspgcs::UploadJobState::Cancelled;
    ++job.snapshot.jobRevision;
    job.snapshot.updatedAtUtcMs = now;
    RecalculateAggregate(job);
    QueueRouteDeltas(job, updates);
    job.artifactLease.reset();
}

bool ConsoleUploadJobAuthority::FinalizeExecutorCancellation(
    const std::string& uploadJobId,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job)
    {
        error = "Executor cancellation finalization requires an existing Job.";
        return false;
    }
    if (job->snapshot.jobState == dspgcs::UploadJobState::Cancelled)
    {
        error.clear();
        return true;
    }
    if (job->snapshot.jobState != dspgcs::UploadJobState::Cancelling ||
        !job->artifactLease)
    {
        error = "Executor cancellation finalization requires a leased Cancelling Job.";
        return false;
    }
    FinalizeCancellation(*job);
    error.clear();
    return true;
}

bool ConsoleUploadJobAuthority::FinalizeExecutorShutdown(
    const std::string& uploadJobId,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job)
    {
        error = "Executor shutdown finalization requires an existing Job.";
        return false;
    }
    if (job->snapshot.jobState == dspgcs::UploadJobState::Completed ||
        job->snapshot.jobState == dspgcs::UploadJobState::Failed ||
        job->snapshot.jobState == dspgcs::UploadJobState::Cancelled)
    {
        error.clear();
        return true;
    }
    if (!job->artifactLease)
    {
        error = "Executor shutdown finalization requires a leased active Job.";
        return false;
    }
    FinalizeCancellation(*job);
    error.clear();
    return true;
}

bool ConsoleUploadJobAuthority::DestroyFailedJob(const std::string& uploadJobId)
{
    const auto item = jobs_.find(uploadJobId);
    if (item == jobs_.end() ||
        item->second.snapshot.jobState != dspgcs::UploadJobState::Failed)
        return false;
    item->second.artifactLease.reset();
    jobs_.erase(item);
    return true;
}

std::vector<dspgcs::UploadJobRouteStateDeltaDTO>
ConsoleUploadJobAuthority::DrainRouteDeltas()
{
    std::vector<dspgcs::UploadJobRouteStateDeltaDTO> result;
    result.swap(pendingDeltas_);
    return result;
}

const dspgcs::UploadJobSnapshotDTO*
ConsoleUploadJobAuthority::FindCurrentSnapshotForTest(
    const std::string& uploadJobId)
{
    Job* job = FindJob(uploadJobId);
    return job ? &job->snapshot : nullptr;
}

std::optional<UploadJobTargetMetadata>
ConsoleUploadJobAuthority::FindTargetMetadataForTest(
    const std::string& uploadJobId,
    const std::int32_t routeSlotIndex) const
{
    const auto job = jobs_.find(uploadJobId);
    if (job == jobs_.end()) return std::nullopt;
    const auto target = std::lower_bound(
        job->second.targets.begin(), job->second.targets.end(), routeSlotIndex,
        [](const Target& value, const std::int32_t slot)
        { return value.metadata.routeSlotIndex < slot; });
    if (target == job->second.targets.end() ||
        target->metadata.routeSlotIndex != routeSlotIndex)
        return std::nullopt;
    return target->metadata;
}

std::size_t ConsoleUploadJobAuthority::GetFrozenPageRouteCountForTest(
    const std::string& uploadJobId) const
{
    const auto item = jobs_.find(uploadJobId);
    return item == jobs_.end() ? 0 : item->second.pageSnapshot.size();
}

bool ConsoleUploadJobAuthority::InstallSyntheticReadyJobForTest(
    const std::string& uploadJobId,
    const dspgcs::UploadArtifactBindingDTO& artifactBinding,
    const dspgcs::UploadFrozenAssignmentBindingDTO& frozenBinding,
    const dspgcs::UploadStableSourceDTO& stableSource,
    std::vector<dspgcs::UploadRouteStateDTO> routes,
    const std::uint32_t vacantCount,
    const std::uint32_t spareCount,
    const std::uint32_t excludedCount,
    const std::uint32_t maxConcurrentUploads,
    std::string& error)
{
    if (!dspgcs::IsCanonicalUploadJobId(uploadJobId) || routes.empty() ||
        maxConcurrentUploads == 0 || FindJob(uploadJobId))
    {
        error = "Synthetic Job input is invalid.";
        return false;
    }
    auto lease =
        std::make_unique<dronegcs::route_artifact::ArtifactReadLease>();
    bool recovered = false;
    const auto acquire = lease->TryAcquire(
        std::filesystem::u8path(workspaceRoot_).wstring(), recovered, error);
    if (acquire !=
        dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
        return false;
    Job job;
    job.snapshot.uploadJobId = uploadJobId;
    job.snapshot.snapshotToken = MakeCanonicalId();
    job.snapshot.artifactBinding = artifactBinding;
    job.snapshot.frozenAssignmentBinding = frozenBinding;
    job.snapshot.stableSource = stableSource;
    job.snapshot.artifactGateState =
        dspgcs::UploadArtifactGateState::Validated;
    job.snapshot.jobState = dspgcs::UploadJobState::Ready;
    job.snapshot.jobRevision = 1;
    job.snapshot.snapshotSequence = 1;
    job.snapshot.updatedAtUtcMs = UtcNowMs();
    job.snapshot.matchedCount = static_cast<std::uint32_t>(routes.size());
    job.snapshot.vacantCount = vacantCount;
    job.snapshot.spareCount = spareCount;
    job.snapshot.excludedCount = excludedCount;
    job.snapshot.effectiveMaxConcurrentUploads =
        ComputeEffectiveMaxConcurrentTargets(
            maxConcurrentUploads,
            routes.size());
    job.requestedMaxConcurrentUploads = maxConcurrentUploads;
    job.snapshot.frozenMavFtpConfig = FreezeMavFtpConfig(
        runtimeConfig_,
        maxConcurrentUploads,
        job.snapshot.effectiveMaxConcurrentUploads);
    std::sort(routes.begin(), routes.end(),
        [](const auto& left, const auto& right)
        { return left.routeSlotIndex < right.routeSlotIndex; });
    job.targets.reserve(routes.size());
    for (auto& route : routes)
    {
        Target target;
        target.metadata.droneId = route.droneId;
        target.metadata.routeSlotIndex = route.routeSlotIndex;
        target.metadata.fileByteSize = static_cast<std::int64_t>(route.totalBytes);
        target.state = std::move(route);
        job.targets.push_back(std::move(target));
    }
    job.artifactLease = std::move(lease);
    RecalculateAggregate(job);
    jobs_.emplace(uploadJobId, std::move(job));
    error.clear();
    return true;
}

bool ConsoleUploadJobAuthority::InstallSyntheticExecutionBindingForTest(
    const std::string& uploadJobId,
    std::shared_ptr<const AcceptedRouteArtifactV2> artifactSnapshot,
    std::vector<UploadJobTargetMetadata> targets,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job || !job->artifactLease || !artifactSnapshot ||
        targets.size() != job->targets.size())
    {
        error = "Synthetic execution binding input is invalid.";
        return false;
    }
    std::sort(targets.begin(), targets.end(),
        [](const auto& left, const auto& right)
        { return left.routeSlotIndex < right.routeSlotIndex; });
    for (std::size_t index = 0; index < targets.size(); ++index)
    {
        if (targets[index].routeSlotIndex !=
                job->targets[index].state.routeSlotIndex ||
            targets[index].droneId != job->targets[index].state.droneId ||
            targets[index].missionPayloadByteSize !=
                job->targets[index].state.totalBytes)
        {
            error = "Synthetic execution target identity is inconsistent.";
            return false;
        }
        job->targets[index].metadata = targets[index];
    }
    job->artifactSnapshot = std::move(artifactSnapshot);
    job->frozenSnapshot =
        std::make_shared<const FrozenAssignmentAcceptedSnapshot>();
    job->currentDirectoryIdentity = job->artifactSnapshot->currentDirectoryIdentity;
    RecalculateAggregate(*job);
    error.clear();
    return true;
}
