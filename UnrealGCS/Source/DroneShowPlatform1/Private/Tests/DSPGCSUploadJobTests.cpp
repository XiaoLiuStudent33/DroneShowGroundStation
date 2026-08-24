#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DSPGCSUploadJobProtocol.h"
#include "DSPGCSUploadJobStateMirror.h"
#include "DSPGCSUploadJobSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactReadLease.h"
#include "RouteArtifactWorkspaceManager.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
	std::string Id(const char Value) { return std::string(32, Value); }

	dspgcs::UploadArtifactBindingDTO ArtifactBinding()
	{
		dspgcs::UploadArtifactBindingDTO Result;
		Result.artifactSetId = std::string(64, 'a');
		Result.artifactSetDigest.fill(1);
		return Result;
	}

	dspgcs::UploadFrozenAssignmentBindingDTO FrozenBinding()
	{
		dspgcs::UploadFrozenAssignmentBindingDTO Result;
		Result.fleetSessionId = "fleet";
		Result.assignmentAuthoritySessionId = "authority";
		Result.assignmentRevision = 1;
		Result.assignmentDigest.fill(2);
		return Result;
	}

	dspgcs::UploadStableSourceDTO StableSource()
	{
		dspgcs::UploadStableSourceDTO Result;
		Result.sourceDatasetDigest.fill(3);
		Result.normalizedShowOriginLatitudeE7 = 300000000;
		Result.normalizedShowOriginLongitudeE7 = 1200000000;
		Result.routeSlotCount = 2;
		return Result;
	}

	dspgcs::UploadJobSnapshotDTO Snapshot()
	{
		dspgcs::UploadJobSnapshotDTO Result;
		Result.uploadJobId = Id('1');
		Result.snapshotToken = Id('2');
		Result.artifactBinding = ArtifactBinding();
		Result.frozenAssignmentBinding = FrozenBinding();
		Result.stableSource = StableSource();
		Result.artifactGateState = dspgcs::UploadArtifactGateState::Validated;
		Result.jobState = dspgcs::UploadJobState::Running;
		Result.jobRevision = 3;
		Result.snapshotSequence = 4;
		Result.routeDeltaSequence = 7;
		Result.matchedCount = 2;
		Result.waitingCount = 2;
		Result.totalBytes = 200;
		Result.effectiveMaxConcurrentUploads = 2;
		return Result;
	}

	dspgcs::UploadRouteStateDTO Route(const int32 Slot)
	{
		dspgcs::UploadRouteStateDTO Result;
		Result.routeSlotIndex = Slot;
		Result.droneId = "drone-" + std::to_string(Slot);
		Result.totalBytes = 100;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSUploadJobLiveAggregateTest,
	"DroneGCS.Data.DSPGCSUploadJob.LiveAggregate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSUploadJobLiveAggregateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FDSPGCSUploadJobStateMirror Mirror;
	auto Aggregate = Snapshot();
	TestTrue(TEXT("Initial Aggregate applies"), Mirror.ApplySnapshot(Aggregate));

	dspgcs::UploadJobRouteStatePageDTO Page;
	Page.uploadJobId = Aggregate.uploadJobId;
	Page.snapshotToken = Aggregate.snapshotToken;
	Page.jobRevision = Aggregate.jobRevision;
	Page.totalRouteCount = 2;
	Page.nextPageStart = 2;
	Page.routes = { Route(0), Route(1) };
	TestTrue(TEXT("Initial Route page applies"), Mirror.ApplyRoutePage(Page));

	dspgcs::UploadJobRouteStateDeltaDTO Delta;
	Delta.uploadJobId = Aggregate.uploadJobId;
	Delta.jobRevision = Aggregate.jobRevision + 1;
	Delta.deltaSequence = Aggregate.routeDeltaSequence + 1;
	auto Progress = Route(0);
	Progress.state = dspgcs::UploadRouteState::Uploading;
	Progress.bytesCompleted = 25;
	Progress.attemptCount = 1;
	Delta.routes.push_back(Progress);
	bool bDuplicate = false;
	TestTrue(TEXT("ACK progress Delta applies"),
		Mirror.ApplyRouteDelta(Delta, bDuplicate));

	auto Live = Aggregate;
	Live.jobRevision = Delta.jobRevision + 1;
	Live.snapshotSequence++;
	Live.routeDeltaSequence = Delta.deltaSequence;
	Live.bytesCompleted = 25;
	Live.throughputBytesPerSecond = 100;
	Live.throughputWindowMs = 250;
	Live.elapsedMs = 250;
	Live.uploadingCount = 1;
	Live.waitingCount = 1;
	TestTrue(TEXT("Live Aggregate applies"), Mirror.ApplySnapshot(Live));
	const auto* Preserved = Mirror.FindRoute(0);
	TestTrue(TEXT("Aggregate poll preserves current Route page"),
		Mirror.IsRoutePageComplete() && Preserved &&
		Preserved->bytesCompleted == 25 && Preserved->attemptCount == 1);
	const auto* Current = Mirror.GetSnapshot();
	TestTrue(TEXT("Logical progress metrics remain authoritative"),
		Current && Current->bytesCompleted == 25 &&
		Current->totalBytes == 200 &&
		Current->throughputBytesPerSecond == 100 &&
		Current->throughputWindowMs == 250 && Current->elapsedMs == 250);

	auto Stale = Live;
	--Stale.jobRevision;
	TestFalse(TEXT("Stale Aggregate revision is rejected"),
		Mirror.ApplySnapshot(Stale));
	auto RegressedDelta = Live;
	++RegressedDelta.jobRevision;
	--RegressedDelta.routeDeltaSequence;
	TestFalse(TEXT("Aggregate cannot regress Route Delta sequence"),
		Mirror.ApplySnapshot(RegressedDelta));
	auto CrossJob = Live;
	CrossJob.uploadJobId = Id('9');
	TestFalse(TEXT("Cross-Job Aggregate is rejected"),
		Mirror.ApplySnapshot(CrossJob));

	auto Terminal = Live;
	Terminal.jobRevision += 2;
	Terminal.jobState = dspgcs::UploadJobState::Completed;
	Terminal.bytesCompleted = Terminal.totalBytes;
	Terminal.elapsedMs = 9000;
	Terminal.throughputBytesPerSecond = 22;
	Terminal.succeededCount = 2;
	Terminal.uploadingCount = 0;
	Terminal.waitingCount = 0;
	TestTrue(TEXT("Terminal Aggregate applies"), Mirror.ApplySnapshot(Terminal));
	Current = Mirror.GetSnapshot();
	TestTrue(TEXT("Succeeded Aggregate is exact and terminal"),
		Current && Current->jobState == dspgcs::UploadJobState::Completed &&
		Current->bytesCompleted == Current->totalBytes &&
		Current->elapsedMs == 9000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSUploadJobMirrorReliabilityTest,
	"DroneGCS.Data.DSPGCSUploadJob.MirrorReliability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSUploadJobMirrorReliabilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FDSPGCSUploadJobStateMirror Mirror;
	auto Baseline = Snapshot();
	std::vector<std::uint8_t> Bytes;
	std::string Error;
	TestTrue(TEXT("Aggregate encodes"),
		dspgcs::EncodeUploadJobSnapshot(Baseline, Bytes, Error));
	dspgcs::UploadJobSnapshotDTO Decoded;
	TestTrue(TEXT("Aggregate decodes"),
		dspgcs::DecodeUploadJobSnapshot(Bytes, Decoded, Error));
	TestTrue(TEXT("Mirror accepts authoritative snapshot"), Mirror.ApplySnapshot(Decoded));

	dspgcs::UploadJobRouteStatePageDTO Page;
	Page.clientRequestId = Id('3');
	Page.uploadJobId = Baseline.uploadJobId;
	Page.snapshotToken = Baseline.snapshotToken;
	Page.jobRevision = Baseline.jobRevision;
	Page.totalRouteCount = 2;
	Page.pageStart = 0;
	Page.nextPageStart = 2;
	Page.routes = { Route(0), Route(1) };
	TestTrue(TEXT("Consistent page applies"), Mirror.ApplyRoutePage(Page));
	TestTrue(TEXT("Page completes route mirror"),
		Mirror.IsRoutePageComplete() && Mirror.GetRouteStateCount() == 2);
	FDSPGCSUploadJobStateMirror PagingMirror;
	TestTrue(TEXT("Paging mirror accepts aggregate"), PagingMirror.ApplySnapshot(Baseline));
	dspgcs::UploadJobRouteStateDeltaDTO EarlyDelta;
	EarlyDelta.uploadJobId = Baseline.uploadJobId;
	EarlyDelta.jobRevision = Baseline.jobRevision + 1;
	EarlyDelta.deltaSequence = Baseline.routeDeltaSequence + 1;
	auto EarlyChanged = Route(0);
	EarlyChanged.state = dspgcs::UploadRouteState::Uploading;
	EarlyChanged.bytesCompleted = 25;
	EarlyDelta.routes.push_back(EarlyChanged);
	bool bEarlyDuplicate = false;
	TestTrue(TEXT("Delta arriving before frozen page is deferred"),
		PagingMirror.ApplyRouteDelta(EarlyDelta, bEarlyDuplicate));
	TestTrue(TEXT("Frozen page applies after a newer Delta"),
		PagingMirror.ApplyRoutePage(Page));
	const auto* Replayed = PagingMirror.FindRoute(0);
	TestTrue(TEXT("Deferred Delta wins over frozen page state"),
		Replayed && Replayed->state == dspgcs::UploadRouteState::Uploading &&
		Replayed->bytesCompleted == 25);
	TArray<int32> SortedRouteSlots;
	Mirror.CopySortedRouteSlots(SortedRouteSlots);
	TestEqual(TEXT("Sorted route slot count"), SortedRouteSlots.Num(), 2);
	if (SortedRouteSlots.Num() == 2)
	{
		TestEqual(TEXT("First sorted route slot"), SortedRouteSlots[0], 0);
		TestEqual(TEXT("Second sorted route slot"), SortedRouteSlots[1], 1);
	}

	dspgcs::UploadJobRouteStateDeltaDTO Delta;
	Delta.uploadJobId = Baseline.uploadJobId;
	Delta.jobRevision = 4;
	Delta.deltaSequence = 8;
	auto Changed = Route(0);
	Changed.state = dspgcs::UploadRouteState::Uploading;
	Changed.bytesCompleted = 10;
	Delta.routes.push_back(Changed);
	bool bDuplicate = false;
	TestTrue(TEXT("Next ordered absolute Delta applies"),
		Mirror.ApplyRouteDelta(Delta, bDuplicate));
	TestTrue(TEXT("Duplicate Delta is harmless"),
		Mirror.ApplyRouteDelta(Delta, bDuplicate) && bDuplicate);

	auto Gap = Delta;
	Gap.deltaSequence = 10;
	Gap.jobRevision = 5;
	TestFalse(TEXT("Delta gap is rejected"), Mirror.ApplyRouteDelta(Gap, bDuplicate));
	TestTrue(TEXT("Delta gap requests recovery"), Mirror.IsRecoveryRequired());

	auto Recovery = Baseline;
	Recovery.jobRevision = Gap.jobRevision;
	Recovery.snapshotSequence++;
	Recovery.routeDeltaSequence = Gap.deltaSequence;
	TestTrue(TEXT("Authoritative gap recovery snapshot applies"),
		Mirror.ApplySnapshot(Recovery));
	auto ReorderedPage = Page;
	ReorderedPage.jobRevision = Recovery.jobRevision;
	ReorderedPage.pageStart = 1;
	TestFalse(TEXT("Reordered page rejected"), Mirror.ApplyRoutePage(ReorderedPage));
	TestTrue(TEXT("Authoritative snapshot retries after bad page"),
		Mirror.ApplySnapshot(Recovery));
	Mirror.MarkTransportReconnected();
	TestTrue(TEXT("Reconnect requests snapshot"), Mirror.IsRecoveryRequired());
	TestTrue(TEXT("Lost snapshot leaves recovery pending"), Mirror.IsRecoveryRequired());
	TestTrue(TEXT("Retried snapshot recovers mirror"), Mirror.ApplySnapshot(Recovery));
	TestFalse(TEXT("Recovered snapshot clears recovery"), Mirror.IsRecoveryRequired());
	Mirror.MarkPageOpened();
	TestTrue(TEXT("Page reopen requests snapshot"), Mirror.IsRecoveryRequired());
	TestTrue(TEXT("Page reopen snapshot applies"), Mirror.ApplySnapshot(Recovery));
	Mirror.MarkRoutePageStale();
	TestTrue(TEXT("Stale Route page retains aggregate and requests recovery"),
		Mirror.HasAuthoritativeSnapshot() && Mirror.IsRecoveryRequired() &&
		!Mirror.IsRoutePageComplete() && Mirror.GetRouteStateCount() == 0);

	dspgcs::UploadJobCommandAckDTO Ack;
	Ack.clientCommandId = Id('4');
	Ack.uploadJobId = Baseline.uploadJobId;
	Ack.commandType = dspgcs::UploadJobCommandType::Pause;
	Ack.status = dspgcs::UploadJobCommandAckStatus::Accepted;
	Mirror.SetPendingCommandID(Ack.clientCommandId);
	TestTrue(TEXT("Matching ACK completes pending command"), Mirror.ApplyCommandAck(Ack));
	TestTrue(TEXT("Accepted ACK is retained"),
		Mirror.GetLastCommandAck() &&
		Mirror.GetLastCommandAck()->status ==
			dspgcs::UploadJobCommandAckStatus::Accepted);
	TestFalse(TEXT("Duplicate ACK ignored"), Mirror.ApplyCommandAck(Ack));
	Ack.clientCommandId = Id('5');
	TestFalse(TEXT("Late unrelated ACK ignored"), Mirror.ApplyCommandAck(Ack));

	dspgcs::UploadJobCommandAckDTO Rejected = Ack;
	Rejected.clientCommandId = Id('6');
	Rejected.status = dspgcs::UploadJobCommandAckStatus::Rejected;
	Rejected.resultCode = dspgcs::UploadJobErrorCode::StaleJobRevision;
	Rejected.safeDiagnostic = "ExpectedJobRevision is stale.";
	Mirror.SetPendingCommandID(Rejected.clientCommandId);
	TestTrue(TEXT("Rejected ACK completes pending command"),
		Mirror.ApplyCommandAck(Rejected));
	TestTrue(TEXT("Rejected ACK result and diagnostic retained"),
		Mirror.GetLastCommandAck() &&
		Mirror.GetLastCommandAck()->resultCode ==
			dspgcs::UploadJobErrorCode::StaleJobRevision &&
		Mirror.GetLastCommandAck()->safeDiagnostic == Rejected.safeDiagnostic);

	dspgcs::UploadJobSnapshotDTO ValidationFailure;
	ValidationFailure.uploadJobId = Baseline.uploadJobId;
	ValidationFailure.resultCode =
		dspgcs::UploadJobErrorCode::StableSourceDatasetMismatch;
	ValidationFailure.safeDiagnostic =
		"Artifact and Frozen Assignment source dataset digests differ.";
	TestTrue(TEXT("Authoritative validation failure is consumed"),
		Mirror.ApplySnapshot(ValidationFailure));
	TestFalse(TEXT("Validation failure is not an executable Job"),
		Mirror.HasAuthoritativeSnapshot());
	TestTrue(TEXT("Validation failure presentation is retained"),
		Mirror.HasAuthoritativeError() &&
		Mirror.GetLastSnapshotResultCode() ==
			dspgcs::UploadJobErrorCode::StableSourceDatasetMismatch &&
		Mirror.GetLastSnapshotSafeDiagnostic() ==
			ValidationFailure.safeDiagnostic);

	const std::string JobB = Id('7');
	Mirror.PrepareForNewJob(JobB);
	auto SnapshotB = Baseline;
	SnapshotB.uploadJobId = JobB;
	SnapshotB.snapshotToken = Id('8');
	SnapshotB.jobState = dspgcs::UploadJobState::Ready;
	TestTrue(TEXT("Explicit rebind accepts the requested new Job"),
		Mirror.ApplySnapshot(SnapshotB));
	auto Unrelated = SnapshotB;
	Unrelated.uploadJobId = Id('9');
	TestFalse(TEXT("Explicit binding rejects an unrelated Job"),
		Mirror.ApplySnapshot(Unrelated));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSUploadJobClientHardeningTest,
	"DroneGCS.Data.DSPGCSUploadJob.ClientHardening",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSUploadJobClientHardeningTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	auto Configure = [](UDSPGCSUploadJobSubsystem* Client,
		TArray<std::vector<std::uint8_t>>& SentFrames)
	{
		Client->StartAuthorityCaptureForTest = [](
			dspgcs::UploadArtifactBindingDTO& OutArtifact,
			dspgcs::UploadFrozenAssignmentBindingDTO& OutFrozen,
			FString& OutError)
		{
			OutArtifact = ArtifactBinding();
			OutFrozen = FrozenBinding();
			OutError.Reset();
			return true;
		};
		Client->FrameSenderForTest = [&SentFrames](
			const std::vector<std::uint8_t>& Frame)
		{
			SentFrames.Add(Frame);
			return true;
		};
	};
	auto DecodeCommand = [this](const std::vector<std::uint8_t>& Frame,
		dspgcs::UploadJobCommandDTO& Out)
	{
		dspgcs::MessageEnvelope Envelope;
		std::vector<std::uint8_t> Payload;
		std::string Error;
		return TestTrue(TEXT("Command envelope decodes"),
			dspgcs::DecodeEnvelope(Frame, Envelope, Payload)) &&
			TestTrue(TEXT("Command payload decodes"),
				dspgcs::DecodeUploadJobCommand(Payload, Out, Error));
	};
	auto DecodeSnapshotRequest = [this](const std::vector<std::uint8_t>& Frame,
		dspgcs::UploadJobSnapshotRequestDTO& Out)
	{
		dspgcs::MessageEnvelope Envelope;
		std::vector<std::uint8_t> Payload;
		std::string Error;
		return TestTrue(TEXT("Snapshot request envelope decodes"),
			dspgcs::DecodeEnvelope(Frame, Envelope, Payload)) &&
			TestTrue(TEXT("Snapshot request payload decodes"),
				dspgcs::DecodeUploadJobSnapshotRequest(Payload, Out, Error));
	};
	auto DeliverAck = [this](UDSPGCSUploadJobSubsystem* Client,
		const dspgcs::UploadJobCommandDTO& Command,
		const dspgcs::UploadJobCommandAckStatus Status,
		const dspgcs::UploadJobErrorCode ResultCode,
		const char* Diagnostic)
	{
		dspgcs::UploadJobCommandAckDTO Ack;
		Ack.clientCommandId = Command.clientCommandId;
		Ack.uploadJobId = Command.uploadJobId;
		Ack.commandType = Command.commandType;
		Ack.status = Status;
		Ack.resultCode = ResultCode;
		Ack.authoritativeJobRevision = 1;
		Ack.effectiveMaxConcurrentUploads = Command.requestedMaxConcurrentUploads;
		Ack.safeDiagnostic = Diagnostic ? Diagnostic : "";
		std::vector<std::uint8_t> Payload;
		std::string Error;
		TestTrue(TEXT("ACK encodes"), dspgcs::EncodeUploadJobCommandAck(Ack, Payload, Error));
		dspgcs::MessageEnvelope Envelope;
		Envelope.messageType = dspgcs::UploadJobMessageTypes::CommandAck;
		Client->EnqueueIncoming(Envelope, Payload);
		Client->ProcessIncoming();
	};
	auto DeliverSnapshot = [this](UDSPGCSUploadJobSubsystem* Client,
		const dspgcs::UploadJobSnapshotDTO& SnapshotValue)
	{
		std::vector<std::uint8_t> Payload;
		std::string Error;
		TestTrue(TEXT("Snapshot encodes"),
			dspgcs::EncodeUploadJobSnapshot(SnapshotValue, Payload, Error));
		dspgcs::MessageEnvelope Envelope;
		Envelope.messageType = dspgcs::UploadJobMessageTypes::Snapshot;
		Client->EnqueueIncoming(Envelope, Payload);
		Client->ProcessIncoming();
	};
	auto DeliverPage = [this](UDSPGCSUploadJobSubsystem* Client,
		const dspgcs::UploadJobRouteStatePageDTO& PageValue)
	{
		std::vector<std::uint8_t> Payload;
		std::string Error;
		TestTrue(TEXT("Route page encodes"),
			dspgcs::EncodeUploadJobRouteStatePage(PageValue, Payload, Error));
		dspgcs::MessageEnvelope Envelope;
		Envelope.messageType = dspgcs::UploadJobMessageTypes::RouteStatePage;
		Client->EnqueueIncoming(Envelope, Payload);
		Client->ProcessIncoming();
	};
	auto DeliverDelta = [this](UDSPGCSUploadJobSubsystem* Client,
		const dspgcs::UploadJobRouteStateDeltaDTO& DeltaValue)
	{
		std::vector<std::uint8_t> Payload;
		std::string Error;
		TestTrue(TEXT("Route Delta encodes"),
			dspgcs::EncodeUploadJobRouteStateDelta(DeltaValue, Payload, Error));
		dspgcs::MessageEnvelope Envelope;
		Envelope.messageType = dspgcs::UploadJobMessageTypes::RouteStateDelta;
		Client->EnqueueIncoming(Envelope, Payload);
		Client->ProcessIncoming();
	};

	UGameInstance* RejectedGameInstance = NewObject<UGameInstance>(GEngine);
	UDSPGCSUploadJobSubsystem* RejectedClient =
		NewObject<UDSPGCSUploadJobSubsystem>(RejectedGameInstance);
	TArray<std::vector<std::uint8_t>> RejectedFrames;
	Configure(RejectedClient, RejectedFrames);
	FString JobID;
	FString Error;
	TestFalse(TEXT("Concurrency zero is rejected before authority capture"),
		RejectedClient->StartUploadJobFromCurrentAuthorities(0, JobID, Error));
	RejectedClient->StartAuthorityCaptureForTest = [](
		dspgcs::UploadArtifactBindingDTO&,
		dspgcs::UploadFrozenAssignmentBindingDTO&,
		FString& OutError)
	{
		OutError = TEXT("Route Artifact is not ready for upload.");
		return false;
	};
	TestFalse(TEXT("Missing Artifact authority rejects Start"),
		RejectedClient->StartUploadJobFromCurrentAuthorities(2, JobID, Error));
	RejectedClient->StartAuthorityCaptureForTest = [](
		dspgcs::UploadArtifactBindingDTO&,
		dspgcs::UploadFrozenAssignmentBindingDTO&,
		FString& OutError)
	{
		OutError = TEXT("Frozen Assignment has not been accepted by Console.");
		return false;
	};
	TestFalse(TEXT("Non-accepted Frozen authority rejects Start"),
		RejectedClient->StartUploadJobFromCurrentAuthorities(2, JobID, Error));
	Configure(RejectedClient, RejectedFrames);
	TestTrue(TEXT("Authority-safe Start sends schema2 command"),
		RejectedClient->StartUploadJobFromCurrentAuthorities(4, JobID, Error));
	TestTrue(TEXT("Start is observable as in-flight"),
		RejectedClient->GetCommandStatus().bCommandInFlight);
	dspgcs::UploadJobCommandDTO RejectedCommand;
	if (!RejectedFrames.IsEmpty()) DecodeCommand(RejectedFrames[0], RejectedCommand);
	TestTrue(TEXT("Subsystem owns Artifact binding"),
		RejectedCommand.artifactBinding.artifactSetId == ArtifactBinding().artifactSetId &&
		RejectedCommand.artifactBinding.artifactSetDigest == ArtifactBinding().artifactSetDigest);
	TestTrue(TEXT("Subsystem owns Frozen binding"),
		dspgcs::UploadFrozenAssignmentBindingEqual(
			RejectedCommand.frozenAssignmentBinding, FrozenBinding()));
	DeliverAck(RejectedClient, RejectedCommand,
		dspgcs::UploadJobCommandAckStatus::Rejected,
		dspgcs::UploadJobErrorCode::FleetSessionStale,
		"Start FleetSessionID is stale.");
	const FDSPGCSUploadJobCommandStatus RejectedStatus =
		RejectedClient->GetCommandStatus();
	TestFalse(TEXT("Rejected ACK clears in-flight"), RejectedStatus.bCommandInFlight);
	TestTrue(TEXT("Rejected ACK status/result/diagnostic are observable"),
		RejectedStatus.bHasCommandAck &&
		RejectedStatus.LastCommandAckStatus == static_cast<int32>(
			dspgcs::UploadJobCommandAckStatus::Rejected) &&
		RejectedStatus.LastCommandResultCode == static_cast<int32>(
			dspgcs::UploadJobErrorCode::FleetSessionStale) &&
		RejectedStatus.LastSafeDiagnostic.Contains(TEXT("FleetSessionID")));

	UGameInstance* ValidationGameInstance = NewObject<UGameInstance>(GEngine);
	UDSPGCSUploadJobSubsystem* ValidationClient =
		NewObject<UDSPGCSUploadJobSubsystem>(ValidationGameInstance);
	TArray<std::vector<std::uint8_t>> ValidationFrames;
	Configure(ValidationClient, ValidationFrames);
	TestTrue(TEXT("Validation client Start"),
		ValidationClient->StartUploadJobFromCurrentAuthorities(3, JobID, Error));
	dspgcs::UploadJobCommandDTO StartCommand;
	if (!ValidationFrames.IsEmpty()) DecodeCommand(ValidationFrames[0], StartCommand);
	DeliverAck(ValidationClient, StartCommand,
		dspgcs::UploadJobCommandAckStatus::Accepted,
		dspgcs::UploadJobErrorCode::None, "");
	TestTrue(TEXT("Accepted ACK is observable"),
		ValidationClient->GetCommandStatus().bHasCommandAck &&
		ValidationClient->GetCommandStatus().LastCommandAckStatus ==
			static_cast<int32>(dspgcs::UploadJobCommandAckStatus::Accepted));
	dspgcs::UploadJobSnapshotRequestDTO ValidatingRequest;
	if (!ValidationFrames.IsEmpty())
		DecodeSnapshotRequest(ValidationFrames.Last(), ValidatingRequest);
	auto Validating = Snapshot();
	Validating.clientRequestId = ValidatingRequest.clientRequestId;
	Validating.uploadJobId = StartCommand.uploadJobId;
	Validating.jobState = dspgcs::UploadJobState::Validating;
	Validating.artifactGateState = dspgcs::UploadArtifactGateState::Validating;
	Validating.matchedCount = 0;
	Validating.waitingCount = 0;
	DeliverSnapshot(ValidationClient, Validating);
	TestTrue(TEXT("Validating snapshot is executable-pending presentation"),
		ValidationClient->Mirror.HasAuthoritativeSnapshot());
	TestTrue(TEXT("Request validation outcome snapshot"),
		ValidationClient->RequestAuthoritativeSnapshot(Error));
	dspgcs::UploadJobSnapshotRequestDTO FailureRequest;
	DecodeSnapshotRequest(ValidationFrames.Last(), FailureRequest);
	dspgcs::UploadJobSnapshotDTO Failure;
	Failure.clientRequestId = FailureRequest.clientRequestId;
	Failure.uploadJobId = StartCommand.uploadJobId;
	Failure.resultCode = dspgcs::UploadJobErrorCode::StableSourceOriginMismatch;
	Failure.safeDiagnostic = "Artifact and Frozen Assignment origins differ.";
	DeliverSnapshot(ValidationClient, Failure);
	const FDSPGCSUploadJobCommandStatus FailureStatus =
		ValidationClient->GetCommandStatus();
	TestTrue(TEXT("Validation failure is authoritative presentation"),
		FailureStatus.bHasAuthoritativeFailure &&
		FailureStatus.LastCommandResultCode == static_cast<int32>(
			dspgcs::UploadJobErrorCode::StableSourceOriginMismatch));
	TestFalse(TEXT("Validation failure terminates executable Validating Job"),
		ValidationClient->Mirror.HasAuthoritativeSnapshot());
	TestTrue(TEXT("Explicit error rebind preparation succeeds"),
		ValidationClient->PrepareForNewUploadJob(Error));
	TestFalse(TEXT("Prepare clears active Job binding"),
		ValidationClient->GetCommandStatus().bHasActiveJobBinding);
	TestTrue(TEXT("Job B Start after explicit rebind"),
		ValidationClient->StartUploadJobFromCurrentAuthorities(5, JobID, Error));
	dspgcs::UploadJobCommandDTO JobBCommand;
	DecodeCommand(ValidationFrames.Last(), JobBCommand);
	DeliverAck(ValidationClient, JobBCommand,
		dspgcs::UploadJobCommandAckStatus::Accepted,
		dspgcs::UploadJobErrorCode::None, "");
	dspgcs::UploadJobSnapshotRequestDTO JobBRequest;
	DecodeSnapshotRequest(ValidationFrames.Last(), JobBRequest);
	auto JobBSnapshot = Snapshot();
	JobBSnapshot.clientRequestId = JobBRequest.clientRequestId;
	JobBSnapshot.uploadJobId = JobBCommand.uploadJobId;
	JobBSnapshot.jobState = dspgcs::UploadJobState::Ready;
	DeliverSnapshot(ValidationClient, JobBSnapshot);
	TestTrue(TEXT("Job B authoritative Snapshot binds after explicit reset"),
		ValidationClient->Mirror.GetSnapshot() &&
		ValidationClient->Mirror.GetSnapshot()->uploadJobId == JobBCommand.uploadJobId);
	dspgcs::UploadJobSnapshotRequestDTO InitialPageRequest;
	DecodeSnapshotRequest(ValidationFrames.Last(), InitialPageRequest);
	TestTrue(TEXT("Ready aggregate requests Route page"),
		InitialPageRequest.requestKind ==
			dspgcs::UploadJobSnapshotRequestKind::RoutePage);
	dspgcs::UploadJobSnapshotDTO StalePageError;
	StalePageError.clientRequestId = InitialPageRequest.clientRequestId;
	StalePageError.uploadJobId = JobBCommand.uploadJobId;
	StalePageError.resultCode = dspgcs::UploadJobErrorCode::StaleJobRevision;
	StalePageError.safeDiagnostic = "Route page snapshot is unavailable or stale.";
	DeliverSnapshot(ValidationClient, StalePageError);
	TestTrue(TEXT("Stale Route page preserves mirror for automatic recovery"),
		ValidationClient->Mirror.HasAuthoritativeSnapshot() &&
		ValidationClient->Mirror.IsRecoveryRequired());
	const int32 FramesBeforeRecovery = ValidationFrames.Num();
	ValidationClient->TickClient(0.0f);
	TestTrue(TEXT("Stale Route page automatically requests Aggregate"),
		ValidationFrames.Num() == FramesBeforeRecovery + 1);
	dspgcs::UploadJobSnapshotRequestDTO RecoveryRequest;
	DecodeSnapshotRequest(ValidationFrames.Last(), RecoveryRequest);
	TestTrue(TEXT("Automatic recovery request is Aggregate"),
		RecoveryRequest.requestKind ==
			dspgcs::UploadJobSnapshotRequestKind::Aggregate);
	auto RecoveredSnapshot = JobBSnapshot;
	RecoveredSnapshot.clientRequestId = RecoveryRequest.clientRequestId;
	RecoveredSnapshot.snapshotToken = Id('b');
	RecoveredSnapshot.jobRevision++;
	DeliverSnapshot(ValidationClient, RecoveredSnapshot);
	dspgcs::UploadJobSnapshotRequestDTO RecoveredPageRequest;
	DecodeSnapshotRequest(ValidationFrames.Last(), RecoveredPageRequest);
	dspgcs::UploadJobRouteStatePageDTO RecoveredPage;
	RecoveredPage.clientRequestId = RecoveredPageRequest.clientRequestId;
	RecoveredPage.uploadJobId = RecoveredSnapshot.uploadJobId;
	RecoveredPage.snapshotToken = RecoveredSnapshot.snapshotToken;
	RecoveredPage.jobRevision = RecoveredSnapshot.jobRevision;
	RecoveredPage.totalRouteCount = 2;
	RecoveredPage.pageStart = 0;
	RecoveredPage.nextPageStart = 2;
	RecoveredPage.routes = { Route(0), Route(1) };
	DeliverPage(ValidationClient, RecoveredPage);
	TestTrue(TEXT("Automatic recovery completes without page reopen"),
		ValidationClient->Mirror.IsRoutePageComplete() &&
		!ValidationClient->Mirror.IsRecoveryRequired());
	dspgcs::UploadJobRouteStateDeltaDTO TerminalDelta;
	TerminalDelta.uploadJobId = RecoveredSnapshot.uploadJobId;
	TerminalDelta.jobRevision = RecoveredSnapshot.jobRevision + 1;
	TerminalDelta.deltaSequence = RecoveredSnapshot.routeDeltaSequence + 1;
	auto SucceededRoute0 = Route(0);
	SucceededRoute0.state = dspgcs::UploadRouteState::Succeeded;
	SucceededRoute0.bytesCompleted = SucceededRoute0.totalBytes;
	auto SucceededRoute1 = Route(1);
	SucceededRoute1.state = dspgcs::UploadRouteState::Succeeded;
	SucceededRoute1.bytesCompleted = SucceededRoute1.totalBytes;
	TerminalDelta.routes = { SucceededRoute0, SucceededRoute1 };
	const int32 FramesBeforeTerminalRefresh = ValidationFrames.Num();
	DeliverDelta(ValidationClient, TerminalDelta);
	ValidationClient->TickClient(0.0f);
	TestTrue(TEXT("Ordered terminal Delta automatically requests Aggregate"),
		ValidationFrames.Num() == FramesBeforeTerminalRefresh + 1);
	dspgcs::UploadJobSnapshotRequestDTO TerminalRequest;
	DecodeSnapshotRequest(ValidationFrames.Last(), TerminalRequest);
	TestTrue(TEXT("Terminal refresh request is Aggregate"),
		TerminalRequest.requestKind ==
			dspgcs::UploadJobSnapshotRequestKind::Aggregate);
	auto StaleRunningSnapshot = JobBSnapshot;
	StaleRunningSnapshot.clientRequestId = TerminalRequest.clientRequestId;
	StaleRunningSnapshot.jobState = dspgcs::UploadJobState::Running;
	DeliverSnapshot(ValidationClient, StaleRunningSnapshot);
	TestTrue(TEXT("Older Running Snapshot cannot overwrite terminal Route revision"),
		ValidationClient->Mirror.GetSnapshot() &&
		ValidationClient->Mirror.GetSnapshot()->jobRevision == TerminalDelta.jobRevision);
	const int32 FramesBeforeStaleReplacement = ValidationFrames.Num();
	ValidationClient->TickClient(0.0f);
	TestTrue(TEXT("Stale terminal Aggregate is replaced by a bounded fresh request"),
		ValidationFrames.Num() == FramesBeforeStaleReplacement + 1);
	DecodeSnapshotRequest(ValidationFrames.Last(), TerminalRequest);
	auto CompletedSnapshot = JobBSnapshot;
	CompletedSnapshot.clientRequestId = TerminalRequest.clientRequestId;
	CompletedSnapshot.jobState = dspgcs::UploadJobState::Completed;
	CompletedSnapshot.jobRevision = TerminalDelta.jobRevision;
	CompletedSnapshot.snapshotToken = Id('a');
	CompletedSnapshot.routeDeltaSequence = TerminalDelta.deltaSequence;
	CompletedSnapshot.waitingCount = 0;
	CompletedSnapshot.succeededCount = 2;
	CompletedSnapshot.bytesCompleted = CompletedSnapshot.totalBytes;
	CompletedSnapshot.throughputBytesPerSecond = 22652;
	CompletedSnapshot.throughputWindowMs = 1000;
	CompletedSnapshot.elapsedMs = 8829;
	DeliverSnapshot(ValidationClient, CompletedSnapshot);
	const FDSPGCSUploadJobMirrorStatus CompletedStatus = ValidationClient->GetMirrorStatus();
	TestEqual(TEXT("Completed aggregate reaches terminal Job state"),
		CompletedStatus.JobState,
		static_cast<int32>(dspgcs::UploadJobState::Completed));
	TestEqual(TEXT("Terminal logical payload bytes are complete"),
		CompletedStatus.BytesCompleted, CompletedStatus.TotalBytes);
	const dspgcs::UploadJobSnapshotDTO* RefreshedTerminal =
		ValidationClient->Mirror.GetSnapshot();
	TestTrue(TEXT("Terminal elapsed and aggregate throughput are authoritative"),
		RefreshedTerminal && RefreshedTerminal->elapsedMs == 8829 &&
		RefreshedTerminal->throughputBytesPerSecond == 22652);
	TestTrue(TEXT("Completed Job explicitly prepares NoJob state"),
		ValidationClient->PrepareForNewUploadJob(Error));
	const FString CompletedJobID = JobID;
	TestTrue(TEXT("Job C Start after Completed explicit rebind"),
		ValidationClient->StartUploadJobFromCurrentAuthorities(6, JobID, Error));
	TestNotEqual(TEXT("New Job ID differs from completed Job"), JobID, CompletedJobID);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactUploadLeasePromotionGateTest,
	"DroneGCS.Data.RouteArtifact.UploadLeasePromotionGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactUploadLeasePromotionGateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Root = FPaths::Combine(
		FPlatformProcess::UserTempDir(),
		TEXT("DroneGCS-2GB-Lease-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Current")), true);
	std::atomic<bool> bHeld{ false };
	std::atomic<bool> bRelease{ false };
	std::atomic<bool> bOwnerExited{ false };
	std::atomic<bool> bOwnerReleaseTimedOut{ false };
	std::atomic<bool> bAcquireFailed{ false };
	constexpr auto WaitTimeout = std::chrono::seconds(5);
	std::thread Owner([&]
	{
		dronegcs::route_artifact::ArtifactReadLease Lease;
		bool bRecovered = false;
		std::string Error;
		if (Lease.TryAcquire(std::wstring(*Root), bRecovered, Error) ==
			dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
		{
			bHeld.store(true);
			const auto ReleaseDeadline = std::chrono::steady_clock::now() + WaitTimeout;
			while (!bRelease.load() &&
				std::chrono::steady_clock::now() < ReleaseDeadline)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			bOwnerReleaseTimedOut.store(!bRelease.load());
		}
		else
		{
			bAcquireFailed.store(true);
		}
		bOwnerExited.store(true);
	});
	const auto HeldDeadline = std::chrono::steady_clock::now() + WaitTimeout;
	while (!bHeld.load() && !bOwnerExited.load() &&
		std::chrono::steady_clock::now() < HeldDeadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	if (!bHeld.load())
	{
		bRelease.store(true);
		if (Owner.joinable()) Owner.join();
		AddError(bAcquireFailed.load()
			? TEXT("Upload lease owner failed to acquire the test mutex.")
			: TEXT("Timed out waiting for the Upload lease owner."));
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	bool bRecovered = false;
	FString GateError;
	TestFalse(TEXT("Promotion is Busy while Upload lease is held"),
		FRouteArtifactWorkspaceManager::TryAcquireCurrentPromotionGateForTest(
			Root, bRecovered, GateError));
	TestTrue(TEXT("Busy result is explicit"),
		GateError.Contains(TEXT("ArtifactCurrentLeaseBusy")));
	bRelease.store(true);
	if (Owner.joinable()) Owner.join();
	if (bOwnerReleaseTimedOut.load())
	{
		AddError(TEXT("Upload lease owner timed out waiting for release."));
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	TestTrue(TEXT("Promotion gate opens after lease release"),
		FRouteArtifactWorkspaceManager::TryAcquireCurrentPromotionGateForTest(
			Root, bRecovered, GateError));
	TArray<FString> Entries;
	IFileManager::Get().FindFiles(
		Entries, *FPaths::Combine(Root, TEXT("*")), false, true);
	TestEqual(TEXT("Lease creates no historical Artifact archive"), Entries.Num(), 1);
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif
