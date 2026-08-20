#include "RouteUploadManager.h"

#include "ConsoleUI.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace
{
    std::int64_t UtcNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool IsTerminalRoute(const dspgcs::UploadRouteState state)
    {
        return state == dspgcs::UploadRouteState::Succeeded ||
            state == dspgcs::UploadRouteState::Failed ||
            state == dspgcs::UploadRouteState::Cancelled;
    }
}

RouteUploadManager::RouteUploadManager(
    ConsoleUploadJobAuthority& authority,
    DroneManager& droneManager,
    const bool routeUploadEnabled)
    : authority_(authority)
    , droneManager_(droneManager)
    , routeUploadEnabled_(routeUploadEnabled)
{
}

RouteUploadManager::~RouteUploadManager()
{
    Shutdown();
}

void RouteUploadManager::Shutdown()
{
    if (shuttingDown_) return;
    shuttingDown_ = true;
    preparationWorker_.CancelActive();
    std::string activeJobId;
    if (activeJob_)
    {
        activeJobId = activeJob_->view.uploadJobId;
        for (RuntimeTarget& target : activeJob_->targets)
        {
            if (target.transaction) target.transaction->Cancel();
            if (target.session)
                target.session->SetMissionProtocolMessageSink(nullptr);
            target.transaction.reset();
            target.session = nullptr;
            target.prepared.clear();
        }
    }
    preparationWorker_.Shutdown();
    if (!activeJobId.empty())
    {
        std::string error;
        authority_.FinalizeExecutorShutdown(activeJobId, error);
    }
    activeJob_.reset();
}

std::uint64_t RouteUploadManager::ElapsedMs(
    const std::chrono::steady_clock::time_point now) const
{
    if (!activeJob_) return 0;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(now - activeJob_->startedAt).count());
}

std::uint64_t RouteUploadManager::LogicalThroughput(
    const std::chrono::steady_clock::time_point now) const
{
    const std::uint64_t elapsed = ElapsedMs(now);
    if (elapsed == 0 || !activeJob_) return 0;
    std::uint64_t bytes = 0;
    for (const RuntimeTarget& target : activeJob_->targets)
        bytes += target.state.bytesCompleted;
    return bytes * 1000 / elapsed;
}

std::size_t RouteUploadManager::GetActiveSlotCountForTest() const
{
    if (!activeJob_) return 0;
    return static_cast<std::size_t>(std::count_if(
        activeJob_->targets.begin(), activeJob_->targets.end(),
        [](const RuntimeTarget& target)
        {
            return target.state.state == dspgcs::UploadRouteState::Preparing ||
                target.state.state == dspgcs::UploadRouteState::Uploading;
        }));
}

std::size_t RouteUploadManager::GetPreparedMissionCountForTest() const
{
    if (!activeJob_) return 0;
    return static_cast<std::size_t>(std::count_if(
        activeJob_->targets.begin(), activeJob_->targets.end(),
        [](const RuntimeTarget& target)
        { return !target.prepared.empty() || target.transaction != nullptr; }));
}

bool RouteUploadManager::AttachJob(
    const std::string& uploadJobId,
    const std::chrono::steady_clock::time_point now)
{
    const auto view = authority_.GetExecutionView(uploadJobId);
    const auto states = authority_.GetExecutorRouteStates(uploadJobId);
    if (!view || view->targets.empty() || states.size() != view->targets.size())
        return false;
    RuntimeJob job;
    job.view = *view;
    job.startedAt = now;
    job.targets.reserve(view->targets.size());
    for (std::size_t index = 0; index < view->targets.size(); ++index)
    {
        const UploadJobTargetMetadata& metadata = view->targets[index];
        const dspgcs::UploadRouteStateDTO& state = states[index];
        if (state.routeSlotIndex != metadata.routeSlotIndex ||
            state.droneId != metadata.droneId)
            return false;
        RuntimeTarget target;
        target.metadata = metadata;
        target.state = state;
        target.metrics.droneId = metadata.droneId;
        target.metrics.routeSlotIndex = metadata.routeSlotIndex;
        target.metrics.missionItemCount = metadata.missionItemCount;
        target.metrics.logicalPayloadBytes = metadata.missionPayloadByteSize;
        job.targets.push_back(std::move(target));
    }
    activeJob_ = std::move(job);
    jobMetrics_ = {};
    jobMetrics_.targetCount = activeJob_->targets.size();
    targetMetrics_.clear();
    return true;
}

void RouteUploadManager::DetachJob()
{
    if (!activeJob_) return;
    for (RuntimeTarget& target : activeJob_->targets)
    {
        if (target.session)
            target.session->SetMissionProtocolMessageSink(nullptr);
        target.transaction.reset();
        target.prepared.clear();
    }
    activeJob_.reset();
}

RouteUploadManager::RuntimeTarget* RouteUploadManager::FindRuntimeTarget(
    const std::int32_t routeSlotIndex)
{
    if (!activeJob_) return nullptr;
    const auto item = std::find_if(activeJob_->targets.begin(),
        activeJob_->targets.end(), [routeSlotIndex](const RuntimeTarget& value)
        { return value.metadata.routeSlotIndex == routeSlotIndex; });
    return item == activeJob_->targets.end() ? nullptr : &*item;
}

void RouteUploadManager::ApplyTarget(
    RuntimeTarget& target,
    const std::chrono::steady_clock::time_point now)
{
    target.state.updatedAtUtcMs = UtcNowMs();
    std::string error;
    authority_.ApplyExecutorRouteUpdates(activeJob_->view.uploadJobId,
        { target.state }, LogicalThroughput(now), 1000, ElapsedMs(now), error);
}

void RouteUploadManager::FailTarget(
    RuntimeTarget& target,
    const dspgcs::RouteUploadErrorCode code,
    const char* diagnostic,
    const bool retryable,
    const std::chrono::steady_clock::time_point now)
{
    if (target.session)
        target.session->SetMissionProtocolMessageSink(nullptr);
    target.session = nullptr;
    target.transaction.reset();
    target.prepared.clear();
    target.state.state = dspgcs::UploadRouteState::Failed;
    target.state.errorCode = code;
    target.state.retryable = retryable;
    target.state.safeFailureSummary = diagnostic ? diagnostic : "Route upload failed.";
    target.metrics.result = "Failed";
    target.metrics.safeDiagnostic = target.state.safeFailureSummary;
    target.metrics.totalDurationMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - target.admittedAt).count());
    ApplyTarget(target, now);
    ConsoleUI::AddLog("[RouteUpload] Target failed DroneID=" +
        target.metadata.droneId + " RouteSlot=" +
        std::to_string(target.metadata.routeSlotIndex) + " " +
        target.state.safeFailureSummary);
}

bool RouteUploadManager::StartPrepared(
    RuntimeTarget& target,
    const std::chrono::steady_clock::time_point now)
{
    if (target.prepared.empty()) return false;
    DroneSession* session = droneManager_.FindDrone(target.metadata.droneId);
    if (!session || !session->GetFlightData().online ||
        !session->GetConfig().missionUploadServiceEnabled)
    {
        FailTarget(target, dspgcs::RouteUploadErrorCode::TransportUnavailable,
            "Target Drone session is offline or Mission upload is disabled.",
            true, now);
        return false;
    }
    MissionUploadTransactionIdentity identity;
    identity.uploadJobId = activeJob_->view.uploadJobId;
    identity.droneId = target.metadata.droneId;
    identity.routeSlotIndex = target.metadata.routeSlotIndex;
    identity.peer = session->GetMissionProtocolPeerIdentity();
    identity.transactionGeneration = target.generation;
    target.session = session;
    target.transaction = std::make_unique<MissionUploadTransaction>(
        std::move(identity), std::move(target.prepared), *session);
    session->SetMissionProtocolMessageSink(target.transaction.get());
    target.state.state = dspgcs::UploadRouteState::Uploading;
    target.state.errorCode = dspgcs::RouteUploadErrorCode::None;
    target.state.retryable = false;
    target.state.safeFailureSummary.clear();
    target.uploadStartedAt = now;
    ApplyTarget(target, now);
    ConsoleUI::AddLog("[RouteUpload] Target start DroneID=" +
        target.metadata.droneId + " RouteSlot=" +
        std::to_string(target.metadata.routeSlotIndex) + " Items=" +
        std::to_string(target.metadata.missionItemCount));
    if (!target.transaction->Start(now))
    {
        const std::string diagnostic = target.transaction->GetSafeDiagnostic();
        FailTarget(target, dspgcs::RouteUploadErrorCode::TransportUnavailable,
            diagnostic.c_str(), true, now);
        return false;
    }
    return true;
}

void RouteUploadManager::ProcessPreparation(
    const std::chrono::steady_clock::time_point now,
    const dspgcs::UploadJobState jobState,
    const std::uint32_t maxConcurrency)
{
    MissionPreparationResult result;
    if (preparationWorker_.Poll(result))
    {
        RuntimeTarget* target = FindRuntimeTarget(result.target.routeSlotIndex);
        if (target && result.uploadJobId == activeJob_->view.uploadJobId &&
            result.generation == target->generation &&
            target->state.state == dspgcs::UploadRouteState::Preparing)
        {
            target->metrics.prepareDurationMs = result.prepareDurationMs;
            if (!result.Succeeded())
            {
                const auto code = result.error == MissionPreparationError::RouteMissing
                    ? dspgcs::RouteUploadErrorCode::RouteFileUnavailable
                    : result.error == MissionPreparationError::WaypointValidationFailed
                    ? dspgcs::RouteUploadErrorCode::WaypointValidationFailed
                    : result.error == MissionPreparationError::MissionConversionFailed
                    ? dspgcs::RouteUploadErrorCode::MissionEncodingFailed
                    : dspgcs::RouteUploadErrorCode::RouteIdentityMismatch;
                FailTarget(*target, code, result.safeDiagnostic.c_str(), true, now);
            }
            else
            {
                target->prepared = std::move(result.mission);
                ConsoleUI::AddLog("[RouteUpload] Prepare complete DroneID=" +
                    target->metadata.droneId + " RouteSlot=" +
                    std::to_string(target->metadata.routeSlotIndex) + " Items=" +
                    std::to_string(target->prepared.size()));
                if (jobState == dspgcs::UploadJobState::Running &&
                    GetActiveSlotCountForTest() <= maxConcurrency)
                    StartPrepared(*target, now);
            }
        }
    }

    if (jobState == dspgcs::UploadJobState::Running)
    {
        for (RuntimeTarget& target : activeJob_->targets)
        {
            if (!target.prepared.empty() && !target.transaction &&
                GetActiveSlotCountForTest() <= maxConcurrency)
                StartPrepared(target, now);
        }
    }
}

void RouteUploadManager::ProcessTransactions(
    const std::chrono::steady_clock::time_point now)
{
    for (RuntimeTarget& target : activeJob_->targets)
    {
        if (!target.transaction) continue;
        const MissionUploadPhase before = target.transaction->GetPhase();
        target.transaction->Tick(now);
        if (target.transaction->ConsumeProgressChanged())
        {
            target.state.bytesCompleted = std::min<std::uint64_t>(
                target.state.totalBytes,
                target.transaction->GetMetrics().logicalBytesCompleted);
            ApplyTarget(target, now);
        }
        const MissionUploadPhase after = target.transaction->GetPhase();
        if (before == MissionUploadPhase::AwaitingAck &&
            after == MissionUploadPhase::VerifyingCount)
            target.verificationStartedAt = now;
        if (!target.transaction->IsTerminal()) continue;

        const MissionUploadTransactionMetrics tx = target.transaction->GetMetrics();
        target.metrics.wireBytesSent = tx.wireBytesSent;
        target.metrics.missionMessagesSent = tx.missionMessagesSent;
        target.metrics.missionRequestsReceived = tx.missionRequestsReceived;
        target.metrics.duplicateRequests = tx.duplicateRequests;
        target.metrics.retryCount = tx.retryCount;
        target.metrics.uploadDurationMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                (target.verificationStartedAt == std::chrono::steady_clock::time_point{}
                    ? now : target.verificationStartedAt) - target.uploadStartedAt).count());
        if (target.verificationStartedAt != std::chrono::steady_clock::time_point{})
            target.metrics.verificationDurationMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - target.verificationStartedAt).count());
        const bool succeeded = target.transaction->Succeeded();
        const std::string diagnostic = target.transaction->GetSafeDiagnostic();
        target.session->SetMissionProtocolMessageSink(nullptr);
        target.session = nullptr;
        target.transaction.reset();
        if (succeeded)
        {
            target.state.state = dspgcs::UploadRouteState::Succeeded;
            target.state.bytesCompleted = target.state.totalBytes;
            target.state.errorCode = dspgcs::RouteUploadErrorCode::None;
            target.state.retryable = false;
            target.state.safeFailureSummary.clear();
            target.metrics.result = "Succeeded";
            target.metrics.totalDurationMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - target.admittedAt).count());
            ApplyTarget(target, now);
            ConsoleUI::AddLog("[RouteUpload] Target verified DroneID=" +
                target.metadata.droneId + " RouteSlot=" +
                std::to_string(target.metadata.routeSlotIndex));
        }
        else
        {
            const auto code = diagnostic.find("timed out") != std::string::npos
                ? dspgcs::RouteUploadErrorCode::UploadTimedOut
                : dspgcs::RouteUploadErrorCode::UploadRejected;
            FailTarget(target, code, diagnostic.c_str(), true, now);
        }
    }
}

void RouteUploadManager::AdmitPreparation(
    const std::chrono::steady_clock::time_point now,
    const std::uint32_t maxConcurrency)
{
    if (preparationWorker_.IsBusy() ||
        GetActiveSlotCountForTest() >= maxConcurrency)
        return;
    const auto item = std::find_if(activeJob_->targets.begin(),
        activeJob_->targets.end(), [](const RuntimeTarget& target)
        { return target.state.state == dspgcs::UploadRouteState::Waiting; });
    if (item == activeJob_->targets.end()) return;
    RuntimeTarget& target = *item;
    target.state.state = dspgcs::UploadRouteState::Preparing;
    target.state.bytesCompleted = 0;
    target.state.errorCode = dspgcs::RouteUploadErrorCode::None;
    target.state.retryable = false;
    target.state.safeFailureSummary.clear();
    ++target.state.attemptCount;
    target.generation = ++activeJob_->nextGeneration;
    target.admittedAt = now;
    ApplyTarget(target, now);
    MissionPreparationRequest request;
    request.uploadJobId = activeJob_->view.uploadJobId;
    request.generation = target.generation;
    request.artifactSnapshot = activeJob_->view.artifactSnapshot;
    request.target = target.metadata;
    if (!preparationWorker_.Submit(std::move(request)))
        FailTarget(target, dspgcs::RouteUploadErrorCode::InternalError,
            "Mission preparation worker is unavailable.", true, now);
}

void RouteUploadManager::ProcessCancellation(
    const std::chrono::steady_clock::time_point now)
{
    (void)now;
    preparationWorker_.CancelActive();
    for (RuntimeTarget& target : activeJob_->targets)
    {
        if (target.transaction) target.transaction->Cancel();
        if (target.session)
            target.session->SetMissionProtocolMessageSink(nullptr);
        target.transaction.reset();
        target.session = nullptr;
        target.prepared.clear();
    }
    MissionPreparationResult discarded;
    preparationWorker_.Poll(discarded);
    if (!preparationWorker_.IsBusy())
    {
        std::string error;
        authority_.FinalizeExecutorCancellation(activeJob_->view.uploadJobId, error);
        DetachJob();
    }
}

void RouteUploadManager::FinalizeJobIfTerminal(
    const dspgcs::UploadJobState state,
    const std::chrono::steady_clock::time_point now)
{
    if (!activeJob_) return;
    const bool allTerminal = std::all_of(activeJob_->targets.begin(),
        activeJob_->targets.end(), [](const RuntimeTarget& target)
        { return IsTerminalRoute(target.state.state); });
    if (!allTerminal) return;
    const bool anyFailed = std::any_of(activeJob_->targets.begin(),
        activeJob_->targets.end(), [](const RuntimeTarget& target)
        { return target.state.state == dspgcs::UploadRouteState::Failed; });
    std::string error;
    if (state == dspgcs::UploadJobState::Running ||
        state == dspgcs::UploadJobState::Paused)
        authority_.TransitionJobForExecutor(activeJob_->view.uploadJobId,
            anyFailed ? dspgcs::UploadJobState::Failed
                      : dspgcs::UploadJobState::Completed,
            error);
    RefreshMetrics(now);
    targetMetrics_.clear();
    for (const RuntimeTarget& target : activeJob_->targets)
        targetMetrics_.push_back(target.metrics);
    ConsoleUI::AddLog("[RouteUpload] Job summary Targets=" +
        std::to_string(jobMetrics_.targetCount) + " Succeeded=" +
        std::to_string(jobMetrics_.succeeded) + " Failed=" +
        std::to_string(jobMetrics_.failed) + " LogicalBytes=" +
        std::to_string(jobMetrics_.logicalBytes) + " WireBytes=" +
        std::to_string(jobMetrics_.wireBytesSent));
    DetachJob();
}

void RouteUploadManager::RefreshMetrics(
    const std::chrono::steady_clock::time_point now)
{
    if (!activeJob_) return;
    jobMetrics_.effectiveMaxConcurrentUploads =
        activeJob_->view.effectiveMaxConcurrentUploads;
    jobMetrics_.elapsedMs = ElapsedMs(now);
    jobMetrics_.succeeded = jobMetrics_.failed = jobMetrics_.cancelled = 0;
    jobMetrics_.logicalBytes = jobMetrics_.wireBytesSent = 0;
    for (const RuntimeTarget& target : activeJob_->targets)
    {
        if (target.state.state == dspgcs::UploadRouteState::Succeeded)
            ++jobMetrics_.succeeded;
        else if (target.state.state == dspgcs::UploadRouteState::Failed)
            ++jobMetrics_.failed;
        else if (target.state.state == dspgcs::UploadRouteState::Cancelled)
            ++jobMetrics_.cancelled;
        jobMetrics_.logicalBytes += target.state.bytesCompleted;
        jobMetrics_.wireBytesSent += target.metrics.wireBytesSent;
    }
    jobMetrics_.aggregateLogicalThroughput = LogicalThroughput(now);
    jobMetrics_.peakActiveUploads = std::max(jobMetrics_.peakActiveUploads,
        static_cast<std::size_t>(std::count_if(activeJob_->targets.begin(),
            activeJob_->targets.end(), [](const RuntimeTarget& target)
            { return target.transaction != nullptr; })));
    jobMetrics_.peakPreparedMissions = std::max(jobMetrics_.peakPreparedMissions,
        GetPreparedMissionCountForTest());
}

void RouteUploadManager::Tick(
    const std::chrono::steady_clock::time_point now)
{
    if (shuttingDown_) return;
    if (!activeJob_)
    {
        const auto candidates = authority_.GetExecutableJobIds();
        if (candidates.empty() || !AttachJob(candidates.front(), now)) return;
    }

    auto snapshot = authority_.GetExecutorSnapshot(activeJob_->view.uploadJobId);
    if (!snapshot)
    {
        DetachJob();
        return;
    }
    activeJob_->view.effectiveMaxConcurrentUploads =
        snapshot->effectiveMaxConcurrentUploads;
    const std::uint32_t maxConcurrency = std::max<std::uint32_t>(
        1, snapshot->effectiveMaxConcurrentUploads);
    if (snapshot->jobState == dspgcs::UploadJobState::Cancelling)
    {
        ProcessCancellation(now);
        return;
    }
    if (snapshot->jobState == dspgcs::UploadJobState::Ready)
    {
        std::string error;
        if (!authority_.TransitionJobForExecutor(activeJob_->view.uploadJobId,
            dspgcs::UploadJobState::Running, error))
            return;
        snapshot = authority_.GetExecutorSnapshot(activeJob_->view.uploadJobId);
        if (!routeUploadEnabled_)
        {
            std::vector<dspgcs::UploadRouteStateDTO> updates;
            updates.reserve(activeJob_->targets.size());
            for (RuntimeTarget& target : activeJob_->targets)
            {
                if (!IsTerminalRoute(target.state.state))
                {
                    target.state.state = dspgcs::UploadRouteState::Failed;
                    target.state.errorCode =
                        dspgcs::RouteUploadErrorCode::TransportUnavailable;
                    target.state.retryable = true;
                    target.state.safeFailureSummary =
                        "Production Mission upload is disabled by configuration.";
                    target.state.updatedAtUtcMs = UtcNowMs();
                    target.metrics.result = "Failed";
                    target.metrics.safeDiagnostic = target.state.safeFailureSummary;
                    updates.push_back(target.state);
                }
            }
            if (!updates.empty())
            {
                std::string updateError;
                authority_.ApplyExecutorRouteUpdates(
                    activeJob_->view.uploadJobId, updates, 0, 1000,
                    ElapsedMs(now), updateError);
            }
            ConsoleUI::AddLog(
                "[RouteUpload] Production Mission upload is disabled; Job failed closed.");
            FinalizeJobIfTerminal(dspgcs::UploadJobState::Running, now);
            return;
        }
    }
    if (!snapshot) return;
    ProcessTransactions(now);
    ProcessPreparation(now, snapshot->jobState, maxConcurrency);
    if (snapshot->jobState == dspgcs::UploadJobState::Running)
        AdmitPreparation(now, maxConcurrency);
    RefreshMetrics(now);
    jobMetrics_.effectiveMaxConcurrentUploads = maxConcurrency;
    FinalizeJobIfTerminal(snapshot->jobState, now);
}
