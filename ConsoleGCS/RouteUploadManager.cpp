#include "RouteUploadManager.h"

#include "MAVFTPMissionTransport.h"

#include "ConsoleUI.h"
#include "MissionProtocolTransport.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
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

	const char* RouteStateName(const dspgcs::UploadRouteState state)
	{
		switch (state)
		{
		case dspgcs::UploadRouteState::Waiting: return "Waiting";
		case dspgcs::UploadRouteState::Preparing: return "Preparing";
		case dspgcs::UploadRouteState::Uploading: return "Uploading";
		case dspgcs::UploadRouteState::RetryWaiting: return "RetryWaiting";
		case dspgcs::UploadRouteState::Succeeded: return "Succeeded";
		case dspgcs::UploadRouteState::Failed: return "Failed";
		case dspgcs::UploadRouteState::Cancelled: return "Cancelled";
		default: return "Unknown";
		}
	}

	const char* RouteErrorName(const dspgcs::RouteUploadErrorCode code)
	{
		switch (code)
		{
		case dspgcs::RouteUploadErrorCode::None: return "None";
		case dspgcs::RouteUploadErrorCode::RouteDescriptorMissing: return "RouteDescriptorMissing";
		case dspgcs::RouteUploadErrorCode::RouteFileUnavailable: return "RouteFileUnavailable";
		case dspgcs::RouteUploadErrorCode::RouteIdentityMismatch: return "RouteIdentityMismatch";
		case dspgcs::RouteUploadErrorCode::WaypointValidationFailed: return "WaypointValidationFailed";
		case dspgcs::RouteUploadErrorCode::MissionEncodingFailed: return "MissionEncodingFailed";
		case dspgcs::RouteUploadErrorCode::TransportUnavailable: return "TransportUnavailable";
		case dspgcs::RouteUploadErrorCode::UploadRejected: return "UploadRejected";
		case dspgcs::RouteUploadErrorCode::UploadTimedOut: return "UploadTimedOut";
		case dspgcs::RouteUploadErrorCode::Cancelled: return "Cancelled";
		case dspgcs::RouteUploadErrorCode::InternalError: return "InternalError";
		default: return "Unknown";
		}
	}

	std::string SafeLogValue(std::string value, const std::size_t maximum)
	{
		if (value.size() > maximum) value.resize(maximum);
		for (char& character : value)
		{
			const unsigned char byte = static_cast<unsigned char>(character);
			if (byte < 0x20 || byte == 0x7f) character = ' ';
		}
		return value;
	}

	void AppendPersistentRouteUploadLog(const std::string& line)
	{
		try
		{
			static std::mutex logMutex;
			std::lock_guard<std::mutex> lock(logMutex);
			wchar_t executablePath[MAX_PATH]{};
			const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
			const std::filesystem::path directory = length > 0 && length < MAX_PATH
				? std::filesystem::path(executablePath).parent_path()
				: std::filesystem::current_path();
			std::ofstream output(directory / "DroneGCS-route-upload.log",
				std::ios::binary | std::ios::app);
			if (!output) return;
			const auto now = std::chrono::system_clock::to_time_t(
				std::chrono::system_clock::now());
			tm local{};
			localtime_s(&local, &now);
			char timestamp[32]{};
			strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &local);
			output << '[' << timestamp << "] " << line << '\n';
		}
		catch (...)
		{
			// Diagnostics must never alter upload authority or terminal semantics.
		}
	}
}

std::unique_ptr<IMissionTransport> CreateConfiguredMissionTransport(
    DroneSession& session,
    const RouteUploadManagerConfig& config,
    std::string& diagnostic)
{
    diagnostic.clear();
    if (config.transportMode == MissionTransportMode::MissionProtocol)
    {
        return CreateMissionProtocolTransport(
            session, session.GetMissionProtocolPeerIdentity());
    }

    dronegcs::mavftp::Client* const client = session.GetMavFtpClient();
    if (!client)
    {
        diagnostic =
            "MAVFTP transport is selected but the endpoint-bound Client is unavailable.";
        return nullptr;
    }
    dronegcs::mission_ftp::MAVFTPMissionTransportConfig transportConfig;
    transportConfig.compileConfig.segmentCapacity = config.mavftpSegmentItems;
    transportConfig.compileConfig.verificationPolicy =
        dronegcs::mission_ftp::VerificationPolicy::ProductionEndpoints;
    transportConfig.driveClientTick = false;
    transportConfig.verificationConfig.responseTimeout =
        std::chrono::milliseconds(config.mavftpRequestTimeoutMs);
    transportConfig.verificationConfig.maxRetries =
        config.mavftpRequestRetryCount;
    return std::make_unique<dronegcs::mission_ftp::MAVFTPMissionTransport>(
        *client, session, session.GetMissionProtocolPeerIdentity(),
        transportConfig);
}

RouteUploadManager::RouteUploadManager(
    ConsoleUploadJobAuthority& authority,
    DroneManager& droneManager,
    const bool routeUploadEnabled)
    : RouteUploadManager(
        authority,
        droneManager,
        RouteUploadManagerConfig{ routeUploadEnabled })
{
}

RouteUploadManager::RouteUploadManager(
    ConsoleUploadJobAuthority& authority,
    DroneManager& droneManager,
    RouteUploadManagerConfig config)
    : authority_(authority)
    , droneManager_(droneManager)
    , routeUploadEnabled_(config.routeUploadEnabled)
    , config_(std::move(config))
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
        const auto now = std::chrono::steady_clock::now();
        for (RuntimeTarget& target : activeJob_->targets)
        {
            if (target.transport) target.transport->Cancel(now);
            if (target.session)
                target.session->SetMissionProtocolMessageSink(nullptr);
            target.transport.reset();
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
        { return !target.prepared.empty() || target.transport != nullptr; }));
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
	lastTerminalDiagnostic_.clear();
	lastJobSummary_.clear();
    return true;
}

void RouteUploadManager::DetachJob()
{
    if (!activeJob_) return;
    for (RuntimeTarget& target : activeJob_->targets)
    {
        if (target.session)
            target.session->SetMissionProtocolMessageSink(nullptr);
        target.transport.reset();
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
	const dspgcs::UploadRouteState sourceState = target.state.state;
    if (target.session)
        target.session->SetMissionProtocolMessageSink(nullptr);
    target.session = nullptr;
    target.transport.reset();
    target.prepared.clear();
    target.state.state = dspgcs::UploadRouteState::Failed;
    target.state.errorCode = code;
    target.state.retryable = retryable;
    target.state.safeFailureSummary = diagnostic ? diagnostic : "Route upload failed.";
    target.metrics.result = "Failed";
	target.metrics.resultCode = code;
	if (target.metrics.terminalPhase.empty())
		target.metrics.terminalPhase = RouteStateName(sourceState);
    target.metrics.safeDiagnostic = target.state.safeFailureSummary;
    target.metrics.totalDurationMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - target.admittedAt).count());
    ApplyTarget(target, now);
    ConsoleUI::AddLog("[RouteUpload] Target failed DroneID=" +
        target.metadata.droneId + " RouteSlot=" +
        std::to_string(target.metadata.routeSlotIndex) + " " +
        target.state.safeFailureSummary);
	PersistTargetTerminal(target);
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
    MissionTransportIdentity identity;
    identity.uploadJobId = activeJob_->view.uploadJobId;
    identity.droneId = target.metadata.droneId;
    identity.routeSlotIndex = target.metadata.routeSlotIndex;
    identity.generation = target.generation;
    target.session = session;
    target.endpoint = session->GetConfig().remoteIp + ":" +
        std::to_string(session->GetConfig().remotePort);
    std::string factoryDiagnostic;
    target.transport = CreateConfiguredMissionTransport(
        *session, config_, factoryDiagnostic);
    if (!target.transport)
    {
        FailTarget(target,
            dspgcs::RouteUploadErrorCode::TransportUnavailable,
            factoryDiagnostic.c_str(), true, now);
        return false;
    }
    std::string prepareDiagnostic;
    if (!target.transport->Prepare(
        std::move(identity), std::move(target.prepared), prepareDiagnostic))
    {
        FailTarget(target, dspgcs::RouteUploadErrorCode::TransportUnavailable,
            prepareDiagnostic.c_str(), true, now);
        return false;
    }
    session->SetMissionProtocolMessageSink(
        target.transport->GetMissionProtocolMessageSink());
    target.state.state = dspgcs::UploadRouteState::Uploading;
    target.state.errorCode = dspgcs::RouteUploadErrorCode::None;
    target.state.retryable = false;
    target.state.safeFailureSummary.clear();
    target.uploadStartedAt = now;
    ApplyTarget(target, now);
    ConsoleUI::AddLog("[RouteUpload] Target start DroneID=" +
        target.metadata.droneId + " Endpoint=" + target.endpoint +
        " RouteSlot=" +
        std::to_string(target.metadata.routeSlotIndex) + " Items=" +
        std::to_string(target.metadata.missionItemCount) + " Transport=" +
        MissionTransportModeName(config_.transportMode));
    if (!target.transport->Upload(now))
    {
        const std::string diagnostic = target.transport->GetSafeDiagnostic();
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
            if (!target.prepared.empty() && !target.transport &&
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
        if (!target.transport) continue;
        target.transport->Tick(now);
        if (config_.transportMode == MissionTransportMode::MAVFTP)
        {
            const auto* mavftpTransport = dynamic_cast<
                const dronegcs::mission_ftp::MAVFTPMissionTransport*>(
                    target.transport.get());
            if (mavftpTransport)
            {
                const auto& mavftp = mavftpTransport->GetMavFtpMetrics();
                if (mavftp.segmentsPlanned > 0 &&
                    target.lastLoggedMavftpSegment != mavftp.activeSegmentIndex)
                {
                    target.lastLoggedMavftpSegment = mavftp.activeSegmentIndex;
                    ConsoleUI::AddLog("[RouteUpload] MAVFTP segment DroneID=" +
                        target.metadata.droneId + " Endpoint=" + target.endpoint +
                        " RouteSlot=" +
                        std::to_string(target.metadata.routeSlotIndex) +
                        " Transport=MAVFTP Segment=" +
                        std::to_string(mavftp.activeSegmentIndex + 1) + "/" +
                        std::to_string(mavftp.segmentsPlanned) + " Committed=" +
                        std::to_string(mavftp.segmentsCommitted));
                }
                if (!target.loggedMavftpVerification &&
                    target.transport->GetStatus() ==
                        MissionTransportStatus::Verifying)
                {
                    target.loggedMavftpVerification = true;
                    ConsoleUI::AddLog("[RouteUpload] Verification start DroneID=" +
                        target.metadata.droneId + " Endpoint=" + target.endpoint +
                        " RouteSlot=" +
                        std::to_string(target.metadata.routeSlotIndex) +
                        " Transport=MAVFTP Checks=Count/Home/First/Last");
                }
            }
        }
        if (target.transport->ConsumeProgressChanged())
        {
            target.state.bytesCompleted = std::min<std::uint64_t>(
                target.state.totalBytes,
                target.transport->GetMetrics().logicalBytesCompleted);
            ApplyTarget(target, now);
        }
        if (!target.transport->IsTerminal()) continue;

        const MissionTransportMetrics tx = target.transport->GetMetrics();
        target.metrics.wireBytesSent = tx.wireBytesSent;
		target.metrics.artifactRouteItems = tx.artifactRouteItems;
		target.metrics.wireMissionItems = tx.wireMissionItems;
		target.metrics.uniqueItemsServed = tx.uniqueItemsServed;
        target.metrics.missionMessagesSent = tx.missionMessagesSent;
        target.metrics.missionRequestsReceived = tx.missionRequestsReceived;
        target.metrics.duplicateRequests = tx.duplicateRequests;
        target.metrics.retryCount = tx.retryCount;
        target.metrics.terminalPhase = target.transport->GetTerminalPhaseName();
        target.metrics.uploadDurationMs = tx.uploadDurationMs;
        target.metrics.verificationDurationMs = tx.verificationDurationMs;
        target.metrics.verificationMismatchDiagnostic =
            target.transport->GetVerificationMismatchDiagnostic();
        const bool succeeded = target.transport->Verify();
        const std::string diagnostic = target.transport->GetSafeDiagnostic();
        if (!succeeded &&
            !target.metrics.verificationMismatchDiagnostic.empty())
        {
            ConsoleUI::AddLog("[RouteUpload] Verification mismatch DroneID=" +
                target.metadata.droneId + " RouteSlot=" +
                std::to_string(target.metadata.routeSlotIndex) + " " +
                SafeLogValue(target.metrics.verificationMismatchDiagnostic,
                    MissionVerificationMismatchDiagnosticMaxBytes));
        }
        target.session->SetMissionProtocolMessageSink(nullptr);
        target.session = nullptr;
        target.transport.reset();
        if (succeeded)
        {
            target.state.state = dspgcs::UploadRouteState::Succeeded;
            target.state.bytesCompleted = target.state.totalBytes;
            target.state.errorCode = dspgcs::RouteUploadErrorCode::None;
            target.state.retryable = false;
            target.state.safeFailureSummary.clear();
            target.metrics.result = "Succeeded";
			target.metrics.resultCode = dspgcs::RouteUploadErrorCode::None;
            target.metrics.totalDurationMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - target.admittedAt).count());
            ApplyTarget(target, now);
            ConsoleUI::AddLog("[RouteUpload] Target verified DroneID=" +
                target.metadata.droneId + " Endpoint=" + target.endpoint +
                " RouteSlot=" +
                std::to_string(target.metadata.routeSlotIndex) + " Transport=" +
                MissionTransportModeName(config_.transportMode) +
                " Result=Succeeded");
			PersistTargetTerminal(target);
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
    preparationWorker_.CancelActive();
    for (RuntimeTarget& target : activeJob_->targets)
    {
        if (target.transport) target.transport->Cancel(now);
        if (target.session)
            target.session->SetMissionProtocolMessageSink(nullptr);
        target.transport.reset();
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
	const auto terminalSnapshot =
		authority_.GetExecutorSnapshot(activeJob_->view.uploadJobId);
    RefreshMetrics(now);
    targetMetrics_.clear();
    for (const RuntimeTarget& target : activeJob_->targets)
        targetMetrics_.push_back(target.metrics);
	PersistJobSummary(terminalSnapshot &&
		!terminalSnapshot->firstFailure.safeSummary.empty()
			? &terminalSnapshot->firstFailure : nullptr);
    DetachJob();
}

void RouteUploadManager::PersistTargetTerminal(const RuntimeTarget& target)
{
	const auto& metrics = target.metrics;
	std::ostringstream line;
	line << "[RouteUpload] Target terminal"
		<< " DroneID=" << SafeLogValue(target.metadata.droneId, 64)
		<< " RouteSlot=" << target.metadata.routeSlotIndex
		<< " Transport=" << MissionTransportModeName(config_.transportMode)
		<< " TerminalPhase=" << SafeLogValue(metrics.terminalPhase, 32)
		<< " ResultCode=" << RouteErrorName(metrics.resultCode)
		<< " SafeDiagnostic=" << SafeLogValue(metrics.safeDiagnostic, 512)
		<< " MissionItems=" << metrics.missionItemCount
		<< " ArtifactRouteItems=" << metrics.artifactRouteItems
		<< " WireMissionItems=" << metrics.wireMissionItems
		<< " UniqueServed=" << metrics.uniqueItemsServed
		<< " MissionRequestsReceived=" << metrics.missionRequestsReceived
		<< " DuplicateRequests=" << metrics.duplicateRequests
		<< " RetryCount=" << metrics.retryCount
		<< " LogicalBytes=" << target.state.bytesCompleted
		<< " WireBytesSent=" << metrics.wireBytesSent
		<< " PrepareDurationMs=" << metrics.prepareDurationMs
		<< " UploadDurationMs=" << metrics.uploadDurationMs
		<< " VerificationDurationMs=" << metrics.verificationDurationMs
		<< " TotalDurationMs=" << metrics.totalDurationMs;
	if (!metrics.verificationMismatchDiagnostic.empty())
	{
		line << ' ' << SafeLogValue(metrics.verificationMismatchDiagnostic,
			MissionVerificationMismatchDiagnosticMaxBytes);
	}
	lastTerminalDiagnostic_ = line.str();
	AppendPersistentRouteUploadLog(lastTerminalDiagnostic_);
}

void RouteUploadManager::PersistJobSummary(
	const dspgcs::UploadJobFailureDTO* const firstFailure)
{
	std::ostringstream line;
	line << "[RouteUpload] Job terminal summary"
		<< " Transport=" << MissionTransportModeName(config_.transportMode)
		<< " Targets=" << jobMetrics_.targetCount
		<< " Succeeded=" << jobMetrics_.succeeded
		<< " Failed=" << jobMetrics_.failed
		<< " Cancelled=" << jobMetrics_.cancelled
		<< " ElapsedMs=" << jobMetrics_.elapsedMs
		<< " LogicalBytes=" << jobMetrics_.logicalBytes
		<< " WireBytesSent=" << jobMetrics_.wireBytesSent
		<< " AggregateLogicalThroughput="
		<< jobMetrics_.aggregateLogicalThroughput
		<< " PeakActiveUploads=" << jobMetrics_.peakActiveUploads
		<< " PeakPreparedMissions=" << jobMetrics_.peakPreparedMissions;
	lastJobSummary_ = line.str();
	ConsoleUI::AddLog(lastJobSummary_);
	AppendPersistentRouteUploadLog(lastJobSummary_);
	if (firstFailure)
	{
		std::ostringstream failure;
		failure << "[RouteUpload] Job firstFailure"
			<< " DroneID=" << SafeLogValue(firstFailure->droneId, 64)
			<< " RouteSlot=" << firstFailure->routeSlotIndex
			<< " ResultCode=" << RouteErrorName(firstFailure->routeErrorCode)
			<< " Retryable=" << (firstFailure->retryable ? "true" : "false")
			<< " SafeDiagnostic=" <<
				SafeLogValue(firstFailure->safeSummary, 512);
		AppendPersistentRouteUploadLog(failure.str());
	}
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
            { return target.transport != nullptr; })));
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
