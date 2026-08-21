#include "DSPGCSUploadJobSubsystem.h"

#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "RouteArtifactTypesV2.h"
#include "RouteArtifactWorkspaceSubsystem.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"

#include <limits>

namespace
{
	std::string UploadJobToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value);
		return std::string(Converted.Get(), Converted.Length());
	}

	FString UploadJobFromUtf8(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	FString UploadJobDigestToHex(const dspgcs::UploadJobDigest& Digest)
	{
		static constexpr TCHAR Digits[] = TEXT("0123456789abcdef");
		FString Result;
		Result.Reserve(64);
		for (const std::uint8_t Byte : Digest)
		{
			Result.AppendChar(Digits[(Byte >> 4) & 0x0f]);
			Result.AppendChar(Digits[Byte & 0x0f]);
		}
		return Result;
	}

	std::int64_t UtcNowMs()
	{
		const FDateTime Epoch(1970, 1, 1);
		return (FDateTime::UtcNow() - Epoch).GetTicks() /
			ETimespan::TicksPerMillisecond;
	}
}

void UDSPGCSUploadJobSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UDSPGCSProductionTelemetryProviderSubsystem>();
	Collection.InitializeDependency<URouteArtifactWorkspaceSubsystem>();
	Collection.InitializeDependency<UDSPGCSFrozenAssignmentSyncSubsystem>();
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		ProviderRef = GameInstance->GetSubsystem<
			UDSPGCSProductionTelemetryProviderSubsystem>();
		WorkspaceRef = GameInstance->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
		FrozenSyncRef =
			GameInstance->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
	}
	ClientSessionID = MakeCanonicalID();
	if (auto* Provider = ProviderRef.Get())
	{
		const TWeakObjectPtr<UDSPGCSUploadJobSubsystem> WeakThis(this);
		Provider->SetUploadJobFrameHandler(
			[WeakThis](const dspgcs::MessageEnvelope& Envelope,
				const std::vector<std::uint8_t>& Payload)
			{
				if (auto* Self = WeakThis.Get()) Self->EnqueueIncoming(Envelope, Payload);
			});
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ThisClass::TickClient),
		ClientTickSeconds);
}

void UDSPGCSUploadJobSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (auto* Provider = ProviderRef.Get()) Provider->SetUploadJobFrameHandler({});
	ProviderRef.Reset();
	WorkspaceRef.Reset();
	FrozenSyncRef.Reset();
	Mirror.Reset();
	PendingCommandFrame.clear();
	ClearPendingSnapshotRequest();
	ActiveJobID.clear();
	bCommandInFlight = false;
	bCommandTimedOut = false;
	bHasCommandAck = false;
	LastSafeDiagnostic.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	StartAuthorityCaptureForTest = {};
	FrameSenderForTest = {};
#endif
	Super::Deinitialize();
}

std::string UDSPGCSUploadJobSubsystem::MakeCanonicalID()
{
	return UploadJobToUtf8(FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
}

bool UDSPGCSUploadJobSubsystem::ParseDigestHex(
	const FString& Hex,
	dspgcs::UploadJobDigest& OutDigest)
{
	if (Hex.Len() != 64) return false;
	for (int32 Index = 0; Index < 32; ++Index)
	{
		auto Nibble = [](const TCHAR Character, uint8& Value)
		{
			if (Character >= TEXT('0') && Character <= TEXT('9'))
				Value = static_cast<uint8>(Character - TEXT('0'));
			else if (Character >= TEXT('a') && Character <= TEXT('f'))
				Value = static_cast<uint8>(Character - TEXT('a') + 10);
			else return false;
			return true;
		};
		uint8 High = 0, Low = 0;
		if (!Nibble(Hex[Index * 2], High) || !Nibble(Hex[Index * 2 + 1], Low))
			return false;
		OutDigest[Index] = static_cast<uint8>((High << 4) | Low);
	}
	return true;
}

bool UDSPGCSUploadJobSubsystem::CaptureCurrentAuthorityBindings(
	dspgcs::UploadArtifactBindingDTO& OutArtifactBinding,
	dspgcs::UploadFrozenAssignmentBindingDTO& OutFrozenBinding,
	FString& OutError)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (StartAuthorityCaptureForTest)
	{
		return StartAuthorityCaptureForTest(
			OutArtifactBinding, OutFrozenBinding, OutError);
	}
#endif
	URouteArtifactWorkspaceSubsystem* Workspace = WorkspaceRef.Get();
	UDSPGCSFrozenAssignmentSyncSubsystem* FrozenSync = FrozenSyncRef.Get();
	if (!Workspace || !FrozenSync)
	{
		OutError = TEXT("Upload Job source authorities are unavailable.");
		return false;
	}
	const ERouteArtifactRecoveryStatus Recovery =
		Workspace->GetLastRecoveryResult().Status;
	if (Workspace->GetCurrentStatus() != ERouteArtifactCurrentStatus::Ready ||
		(Recovery != ERouteArtifactRecoveryStatus::Stable &&
			Recovery != ERouteArtifactRecoveryStatus::Recovered))
	{
		OutError = TEXT("Route Artifact is not ready for upload.");
		return false;
	}
	const FRouteArtifactCurrentInfo& Artifact = Workspace->GetCurrentInfo();
	if (Artifact.ArtifactSchemaVersion != RouteArtifactContractV2::ArtifactSchemaVersion ||
		Artifact.ArtifactSetID.IsEmpty() || !Artifact.ArtifactSetDigest.IsValid())
	{
		OutError = TEXT("Current Route Artifact v2 identity is unavailable.");
		return false;
	}

	FrozenSync->ProcessPendingAcks();
	const FDSPGCSFrozenAssignmentSyncStatus Frozen = FrozenSync->GetSyncStatus();
	if (Frozen.State != EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted)
	{
		OutError = TEXT("Frozen Assignment has not been accepted by Console.");
		return false;
	}
	if (Frozen.FleetSessionID.IsEmpty() ||
		Frozen.AssignmentAuthoritySessionID.IsEmpty() ||
		Frozen.AssignmentRevision <= 0 || Frozen.CanonicalPayloadDigest.IsEmpty())
	{
		OutError = TEXT("Console-accepted Frozen Assignment identity is incomplete.");
		return false;
	}

	OutArtifactBinding = {};
	OutArtifactBinding.artifactSetId = UploadJobToUtf8(Artifact.ArtifactSetID);
	OutFrozenBinding = {};
	OutFrozenBinding.fleetSessionId = UploadJobToUtf8(Frozen.FleetSessionID);
	OutFrozenBinding.assignmentAuthoritySessionId =
		UploadJobToUtf8(Frozen.AssignmentAuthoritySessionID);
	OutFrozenBinding.assignmentRevision =
		static_cast<std::uint64_t>(Frozen.AssignmentRevision);
	if (!ParseDigestHex(Artifact.ArtifactSetDigest.ToLowerHex(),
			OutArtifactBinding.artifactSetDigest) ||
		!ParseDigestHex(Frozen.CanonicalPayloadDigest,
			OutFrozenBinding.assignmentDigest))
	{
		OutError = TEXT("Current Artifact or Frozen Assignment identity is invalid.");
		return false;
	}
	OutError.Reset();
	return true;
}

bool UDSPGCSUploadJobSubsystem::StartUploadJobFromCurrentAuthorities(
	const int32 RequestedMaxConcurrentUploads,
	FString& OutUploadJobID,
	FString& OutError)
{
	check(IsInGameThread());
	OutUploadJobID.Reset();
	if (RequestedMaxConcurrentUploads <= 0)
	{
		OutError = TEXT("Maximum concurrent uploads must be a positive integer.");
		SetLocalCommandFailure(dspgcs::UploadJobCommandType::Start, OutError);
		return false;
	}
	if (bCommandInFlight)
	{
		OutError = TEXT("An Upload Job command is already awaiting ACK.");
		return false;
	}
	if (!ActiveJobID.empty() || Mirror.HasAuthoritativeSnapshot() ||
		Mirror.HasAuthoritativeError())
	{
		OutError = TEXT("The active Upload Job must be explicitly prepared for a new Job.");
		SetLocalCommandFailure(dspgcs::UploadJobCommandType::Start, OutError);
		return false;
	}
	dspgcs::UploadArtifactBindingDTO ArtifactBinding;
	dspgcs::UploadFrozenAssignmentBindingDTO FrozenBinding;
	if (!CaptureCurrentAuthorityBindings(
		ArtifactBinding, FrozenBinding, OutError))
	{
		SetLocalCommandFailure(dspgcs::UploadJobCommandType::Start, OutError);
		return false;
	}
	return StartUploadJob(
		FString(), UploadJobFromUtf8(ArtifactBinding.artifactSetId),
		UploadJobDigestToHex(ArtifactBinding.artifactSetDigest),
		UploadJobFromUtf8(FrozenBinding.fleetSessionId),
		UploadJobFromUtf8(FrozenBinding.assignmentAuthoritySessionId),
		static_cast<int64>(FrozenBinding.assignmentRevision),
		UploadJobDigestToHex(FrozenBinding.assignmentDigest),
		RequestedMaxConcurrentUploads, OutUploadJobID, OutError);
}

bool UDSPGCSUploadJobSubsystem::StartUploadJob(
	const FString& RequestedUploadJobID,
	const FString& ArtifactSetID,
	const FString& ArtifactSetDigestHex,
	const FString& FleetSessionID,
	const FString& AssignmentAuthoritySessionID,
	const int64 AssignmentRevision,
	const FString& AssignmentDigestHex,
	const int32 RequestedMaxConcurrentUploads,
	FString& OutUploadJobID,
	FString& OutError)
{
	check(IsInGameThread());
	if (bCommandInFlight || !ActiveJobID.empty() ||
		Mirror.HasAuthoritativeSnapshot() || Mirror.HasAuthoritativeError())
	{
		OutError = bCommandInFlight
			? TEXT("An Upload Job command is already awaiting ACK.")
			: TEXT("Prepare the terminal or failed Upload Job before starting a new Job.");
		SetLocalCommandFailure(dspgcs::UploadJobCommandType::Start, OutError);
		return false;
	}
	dspgcs::UploadJobCommandDTO Command;
	Command.clientCommandId = MakeCanonicalID();
	Command.uploadJobId = RequestedUploadJobID.IsEmpty()
		? MakeCanonicalID() : UploadJobToUtf8(RequestedUploadJobID);
	Command.commandType = dspgcs::UploadJobCommandType::Start;
	Command.requestedMaxConcurrentUploads = RequestedMaxConcurrentUploads > 0
		? static_cast<std::uint32_t>(RequestedMaxConcurrentUploads) : 0;
	Command.artifactBinding.artifactSetId = UploadJobToUtf8(ArtifactSetID);
	Command.frozenAssignmentBinding.fleetSessionId = UploadJobToUtf8(FleetSessionID);
	Command.frozenAssignmentBinding.assignmentAuthoritySessionId =
		UploadJobToUtf8(AssignmentAuthoritySessionID);
	Command.frozenAssignmentBinding.assignmentRevision = AssignmentRevision > 0
		? static_cast<std::uint64_t>(AssignmentRevision) : 0;
	if (!ParseDigestHex(ArtifactSetDigestHex,
			Command.artifactBinding.artifactSetDigest) ||
		!ParseDigestHex(AssignmentDigestHex,
			Command.frozenAssignmentBinding.assignmentDigest))
	{
		OutError = TEXT("Artifact or Assignment digest is not 64 lowercase hex.");
		SetLocalCommandFailure(dspgcs::UploadJobCommandType::Start, OutError);
		return false;
	}
	if (!SendCommand(Command, OutError)) return false;
	ActiveJobID = Command.uploadJobId;
	Mirror.PrepareForNewJob(ActiveJobID);
	OutUploadJobID = UploadJobFromUtf8(ActiveJobID);
	return true;
}

bool UDSPGCSUploadJobSubsystem::PrepareForNewUploadJob(FString& OutError)
{
	check(IsInGameThread());
	if (bCommandInFlight)
	{
		OutError = TEXT("Cannot prepare a new Upload Job while a command is awaiting ACK.");
		return false;
	}
	const dspgcs::UploadJobSnapshotDTO* Snapshot = Mirror.GetSnapshot();
	const bool bTerminal = Snapshot &&
		(Snapshot->jobState == dspgcs::UploadJobState::Completed ||
		 Snapshot->jobState == dspgcs::UploadJobState::Failed ||
		 Snapshot->jobState == dspgcs::UploadJobState::Cancelled);
	if (!ActiveJobID.empty() && !bTerminal && !Mirror.HasAuthoritativeError())
	{
		OutError = TEXT("Only a terminal or validation-failed Upload Job can be prepared for replacement.");
		return false;
	}
	ClearPendingSnapshotRequest();
	PendingCommandFrame.clear();
	PendingAttemptCount = 0;
	ActiveJobID.clear();
	Mirror.Reset();
	bCommandTimedOut = false;
	bHasCommandAck = false;
	LastCommandAckStatus = dspgcs::UploadJobCommandAckStatus::Rejected;
	LastCommandResultCode = dspgcs::UploadJobErrorCode::None;
	LastSafeDiagnostic.Reset();
	OutError.Reset();
	return true;
}

bool UDSPGCSUploadJobSubsystem::PauseUploadJob(FString& OutError)
{
	return SendSimpleCommand(dspgcs::UploadJobCommandType::Pause, 0, OutError);
}
bool UDSPGCSUploadJobSubsystem::ResumeUploadJob(FString& OutError)
{
	return SendSimpleCommand(dspgcs::UploadJobCommandType::Resume, 0, OutError);
}
bool UDSPGCSUploadJobSubsystem::CancelUploadJob(FString& OutError)
{
	return SendSimpleCommand(dspgcs::UploadJobCommandType::Cancel, 0, OutError);
}
bool UDSPGCSUploadJobSubsystem::RetryFailedRoutes(FString& OutError)
{
	return SendSimpleCommand(dspgcs::UploadJobCommandType::RetryFailed, 0, OutError);
}
bool UDSPGCSUploadJobSubsystem::SetMaxConcurrentUploads(
	const int32 Maximum,
	FString& OutError)
{
	return SendSimpleCommand(
		dspgcs::UploadJobCommandType::SetMaxConcurrentUploads,
		Maximum > 0 ? static_cast<std::uint32_t>(Maximum) : 0,
		OutError);
}

bool UDSPGCSUploadJobSubsystem::SendSimpleCommand(
	const dspgcs::UploadJobCommandType CommandType,
	const std::uint32_t RequestedConcurrency,
	FString& OutError)
{
	check(IsInGameThread());
	const auto* Snapshot = Mirror.GetSnapshot();
	if (!Snapshot || ActiveJobID.empty())
	{
		OutError = TEXT("No authoritative Upload Job snapshot is available.");
		SetLocalCommandFailure(CommandType, OutError);
		return false;
	}
	dspgcs::UploadJobCommandDTO Command;
	Command.clientCommandId = MakeCanonicalID();
	Command.uploadJobId = ActiveJobID;
	Command.commandType = CommandType;
	Command.expectedJobRevision = Snapshot->jobRevision;
	Command.requestedMaxConcurrentUploads = RequestedConcurrency;
	return SendCommand(std::move(Command), OutError);
}

bool UDSPGCSUploadJobSubsystem::SendCommand(
	dspgcs::UploadJobCommandDTO Command,
	FString& OutError)
{
	if (bCommandInFlight || !PendingCommandFrame.empty())
	{
		OutError = TEXT("An Upload Job command is already awaiting ACK.");
		return false;
	}
	CurrentCommandType = Command.commandType;
	bCommandTimedOut = false;
	bHasCommandAck = false;
	LastCommandAckStatus = dspgcs::UploadJobCommandAckStatus::Rejected;
	LastCommandResultCode = dspgcs::UploadJobErrorCode::None;
	LastSafeDiagnostic.Reset();
	std::vector<std::uint8_t> Payload;
	std::string Error;
	if (!dspgcs::EncodeUploadJobCommand(Command, Payload, Error))
	{
		OutError = UploadJobFromUtf8(Error);
		SetLocalCommandFailure(Command.commandType, OutError);
		return false;
	}
	std::vector<std::uint8_t> Frame;
	if (!SendPayload(dspgcs::UploadJobMessageTypes::Command,
		Command.clientCommandId, Payload, &Frame, OutError))
	{
		SetLocalCommandFailure(Command.commandType, OutError);
		return false;
	}
	PendingCommandFrame = std::move(Frame);
	Mirror.SetPendingCommandID(Command.clientCommandId);
	bCommandInFlight = true;
	PendingAttemptCount = 1;
	RetryAtSeconds = FPlatformTime::Seconds() + CommandRetrySeconds;
	return true;
}

bool UDSPGCSUploadJobSubsystem::SendPayload(
	const dspgcs::MessageType MessageType,
	const std::string& CorrelationID,
	const std::vector<std::uint8_t>& Payload,
	std::vector<std::uint8_t>* OutFrame,
	FString& OutError)
{
	dspgcs::MessageEnvelope Envelope;
	Envelope.messageType = MessageType;
	Envelope.sourceId = "UnrealGCS-UploadJob";
	Envelope.sessionId = ClientSessionID;
	Envelope.sequence = ++OutgoingSequence;
	Envelope.timestampUtcMs = UtcNowMs();
	Envelope.correlationId = CorrelationID;
	std::vector<std::uint8_t> Frame = dspgcs::MakeFrame(Envelope, Payload);
	if (Frame.size() > dspgcs::UploadJobDefaultDatagramBudgetBytes)
	{
		OutError = TEXT("Encoded Upload Job DSP frame exceeds safe UDP budget.");
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (FrameSenderForTest)
	{
		if (!FrameSenderForTest(Frame))
		{
			OutError = TEXT("DSP-GCS test transport is unavailable.");
			return false;
		}
		if (OutFrame) *OutFrame = std::move(Frame);
		OutError.Reset();
		return true;
	}
#endif
	auto* Provider = ProviderRef.Get();
	if (!Provider || !Provider->SendFrame(Frame))
	{
		OutError = TEXT("DSP-GCS transport is unavailable.");
		return false;
	}
	if (OutFrame) *OutFrame = std::move(Frame);
	OutError.Reset();
	return true;
}

void UDSPGCSUploadJobSubsystem::SetLocalCommandFailure(
	const dspgcs::UploadJobCommandType CommandType,
	const FString& SafeDiagnostic)
{
	CurrentCommandType = CommandType;
	bCommandInFlight = false;
	bHasCommandAck = false;
	bCommandTimedOut = false;
	LastCommandAckStatus = dspgcs::UploadJobCommandAckStatus::Rejected;
	LastCommandResultCode = dspgcs::UploadJobErrorCode::None;
	LastSafeDiagnostic = SafeDiagnostic.Left(512);
}

void UDSPGCSUploadJobSubsystem::ClearPendingSnapshotRequest()
{
	PendingSnapshotRequestFrame.clear();
	PendingSnapshotRequestID.clear();
	PendingSnapshotRequestKind = dspgcs::UploadJobSnapshotRequestKind::Aggregate;
	PendingSnapshotAttemptCount = 0;
	bSnapshotRequestInFlight = false;
}

bool UDSPGCSUploadJobSubsystem::RequestAuthoritativeSnapshot(FString& OutError)
{
	check(IsInGameThread());
	if (ActiveJobID.empty())
	{
		OutError = TEXT("UploadJobID is not available.");
		return false;
	}
	return SendSnapshotRequest(
		dspgcs::UploadJobSnapshotRequestKind::Aggregate, 0, {}, OutError);
}

bool UDSPGCSUploadJobSubsystem::SendSnapshotRequest(
	const dspgcs::UploadJobSnapshotRequestKind Kind,
	const std::uint32_t PageStart,
	const std::string& SnapshotToken,
	FString& OutError)
{
	dspgcs::UploadJobSnapshotRequestDTO Request;
	Request.clientRequestId = MakeCanonicalID();
	Request.uploadJobId = ActiveJobID;
	Request.requestKind = Kind;
	Request.snapshotToken = SnapshotToken;
	Request.routePageStart = PageStart;
	Request.maximumRouteEntries = Kind ==
		dspgcs::UploadJobSnapshotRequestKind::RoutePage
		? RoutePageRequestMaximum : 0;
	std::vector<std::uint8_t> Payload;
	std::string Error;
	if (!dspgcs::EncodeUploadJobSnapshotRequest(Request, Payload, Error))
	{
		OutError = UploadJobFromUtf8(Error);
		return false;
	}
	PendingSnapshotRequestFrame.clear();
	PendingSnapshotRequestID.clear();
	PendingSnapshotAttemptCount = 0;
	bSnapshotRequestInFlight = false;
	std::vector<std::uint8_t> Frame;
	bSnapshotRequestInFlight = SendPayload(
		dspgcs::UploadJobMessageTypes::SnapshotRequest,
		Request.clientRequestId, Payload, &Frame, OutError);
	if (bSnapshotRequestInFlight)
	{
		PendingSnapshotRequestFrame = std::move(Frame);
		PendingSnapshotRequestID = Request.clientRequestId;
		PendingSnapshotRequestKind = Kind;
		PendingSnapshotAttemptCount = 1;
		SnapshotRetryAtSeconds = FPlatformTime::Seconds() +
			SnapshotRequestRetrySeconds;
	}
	return bSnapshotRequestInFlight;
}

void UDSPGCSUploadJobSubsystem::NotifyUploadPageOpened()
{
	Mirror.MarkPageOpened();
	FString Ignored;
	RequestAuthoritativeSnapshot(Ignored);
}

void UDSPGCSUploadJobSubsystem::EnqueueIncoming(
	const dspgcs::MessageEnvelope& Envelope,
	const std::vector<std::uint8_t>& Payload)
{
	IncomingFrames.Enqueue({ Envelope, Payload });
}

void UDSPGCSUploadJobSubsystem::ProcessIncoming()
{
	FDSPGCSUploadJobIncomingFrame Frame;
	while (IncomingFrames.Dequeue(Frame))
	{
		std::string Error;
		if (Frame.Envelope.messageType == dspgcs::UploadJobMessageTypes::CommandAck)
		{
			dspgcs::UploadJobCommandAckDTO Ack;
			if (dspgcs::DecodeUploadJobCommandAck(Frame.Payload, Ack, Error) &&
				Mirror.ApplyCommandAck(Ack))
			{
				PendingCommandFrame.clear();
				PendingAttemptCount = 0;
				bCommandInFlight = false;
				bCommandTimedOut = false;
				bHasCommandAck = true;
				CurrentCommandType = Ack.commandType;
				LastCommandAckStatus = Ack.status;
				LastCommandResultCode = Ack.resultCode;
				LastSafeDiagnostic = UploadJobFromUtf8(Ack.safeDiagnostic).Left(512);
				FString Ignored;
				RequestAuthoritativeSnapshot(Ignored);
			}
		}
		else if (Frame.Envelope.messageType == dspgcs::UploadJobMessageTypes::Snapshot)
		{
			dspgcs::UploadJobSnapshotDTO Snapshot;
			if (dspgcs::DecodeUploadJobSnapshot(Frame.Payload, Snapshot, Error) &&
				Snapshot.clientRequestId == PendingSnapshotRequestID)
			{
				const bool bStaleRoutePage =
					PendingSnapshotRequestKind ==
						dspgcs::UploadJobSnapshotRequestKind::RoutePage &&
					Snapshot.resultCode ==
						dspgcs::UploadJobErrorCode::StaleJobRevision;
				if (bStaleRoutePage)
				{
					ClearPendingSnapshotRequest();
					Mirror.MarkRoutePageStale();
					NextSnapshotPollSeconds = 0.0;
					continue;
				}
				if (!Mirror.ApplySnapshot(Snapshot)) continue;
				ClearPendingSnapshotRequest();
				if (Snapshot.resultCode != dspgcs::UploadJobErrorCode::None)
				{
					LastCommandResultCode = Snapshot.resultCode;
					LastSafeDiagnostic =
						UploadJobFromUtf8(Snapshot.safeDiagnostic).Left(512);
					NextSnapshotPollSeconds = 0.0;
				}
				else if (Snapshot.jobState == dspgcs::UploadJobState::Validating)
				{
					NextSnapshotPollSeconds = FPlatformTime::Seconds() +
						ValidationSnapshotPollSeconds;
				}
				else if (Snapshot.matchedCount > 0)
				{
					FString Ignored;
					SendSnapshotRequest(
						dspgcs::UploadJobSnapshotRequestKind::RoutePage,
						0, Snapshot.snapshotToken, Ignored);
				}
			}
		}
		else if (Frame.Envelope.messageType ==
			dspgcs::UploadJobMessageTypes::RouteStatePage)
		{
			dspgcs::UploadJobRouteStatePageDTO Page;
			if (dspgcs::DecodeUploadJobRouteStatePage(Frame.Payload, Page, Error) &&
				Page.clientRequestId == PendingSnapshotRequestID &&
				Mirror.ApplyRoutePage(Page))
			{
				ClearPendingSnapshotRequest();
				if (Page.hasMore)
				{
					FString Ignored;
					SendSnapshotRequest(
						dspgcs::UploadJobSnapshotRequestKind::RoutePage,
						Page.nextPageStart, Page.snapshotToken, Ignored);
				}
			}
		}
		else if (Frame.Envelope.messageType ==
			dspgcs::UploadJobMessageTypes::RouteStateDelta)
		{
			dspgcs::UploadJobRouteStateDeltaDTO Delta;
			bool bDuplicate = false;
			if (!dspgcs::DecodeUploadJobRouteStateDelta(Frame.Payload, Delta, Error) ||
				!Mirror.ApplyRouteDelta(Delta, bDuplicate))
			{
				FString Ignored;
				RequestAuthoritativeSnapshot(Ignored);
			}
		}
	}
}

bool UDSPGCSUploadJobSubsystem::TickClient(float DeltaSeconds)
{
	(void)DeltaSeconds;
	ProcessIncoming();
	const double Now = FPlatformTime::Seconds();
	if (!PendingCommandFrame.empty() && Now >= RetryAtSeconds)
	{
		if (PendingAttemptCount >= MaxCommandAttempts)
		{
			PendingCommandFrame.clear();
			PendingAttemptCount = 0;
			bCommandInFlight = false;
			bCommandTimedOut = true;
			bHasCommandAck = false;
			LastCommandResultCode = dspgcs::UploadJobErrorCode::None;
			LastSafeDiagnostic =
				TEXT("Upload Job command timed out waiting for Console ACK.");
			Mirror.ClearPendingCommandID();
			Mirror.MarkTransportReconnected();
			FString Ignored;
			RequestAuthoritativeSnapshot(Ignored);
		}
		else
		{
			if (auto* Provider = ProviderRef.Get()) Provider->SendFrame(PendingCommandFrame);
			++PendingAttemptCount;
			RetryAtSeconds = Now + CommandRetrySeconds;
		}
	}
	if (!PendingSnapshotRequestFrame.empty() && Now >= SnapshotRetryAtSeconds)
	{
		if (PendingSnapshotAttemptCount >= MaxSnapshotRequestAttempts)
		{
			ClearPendingSnapshotRequest();
			Mirror.MarkTransportReconnected();
			NextSnapshotPollSeconds = Now + SnapshotRequestRetrySeconds;
		}
		else
		{
			if (auto* Provider = ProviderRef.Get())
				Provider->SendFrame(PendingSnapshotRequestFrame);
			++PendingSnapshotAttemptCount;
			SnapshotRetryAtSeconds = Now + SnapshotRequestRetrySeconds;
		}
	}
	const auto* Snapshot = Mirror.GetSnapshot();
	if (Mirror.IsRecoveryRequired() && !bSnapshotRequestInFlight &&
		!ActiveJobID.empty() && Now >= NextSnapshotPollSeconds)
	{
		FString Ignored;
		RequestAuthoritativeSnapshot(Ignored);
		NextSnapshotPollSeconds = Now + SnapshotRequestRetrySeconds;
	}
	if (Snapshot && Snapshot->jobState == dspgcs::UploadJobState::Validating &&
		!bSnapshotRequestInFlight && Now >= NextSnapshotPollSeconds)
	{
		FString Ignored;
		RequestAuthoritativeSnapshot(Ignored);
		NextSnapshotPollSeconds = Now + ValidationSnapshotPollSeconds;
	}
	return true;
}

FDSPGCSUploadJobMirrorStatus UDSPGCSUploadJobSubsystem::GetMirrorStatus() const
{
	FDSPGCSUploadJobMirrorStatus Result;
	Result.bHasAuthoritativeSnapshot = Mirror.HasAuthoritativeSnapshot();
	Result.bRecoveryRequired = Mirror.IsRecoveryRequired();
	Result.MirroredRouteCount = Mirror.GetRouteStateCount();
	const auto* Snapshot = Mirror.GetSnapshot();
	if (!Snapshot) return Result;
	Result.UploadJobID = UploadJobFromUtf8(Snapshot->uploadJobId);
	Result.JobState = static_cast<int32>(Snapshot->jobState);
	Result.JobRevision = Snapshot->jobRevision <=
		static_cast<std::uint64_t>(std::numeric_limits<int64>::max())
		? static_cast<int64>(Snapshot->jobRevision) : MAX_int64;
	Result.Matched = static_cast<int32>(Snapshot->matchedCount);
	Result.Waiting = static_cast<int32>(Snapshot->waitingCount);
	Result.Preparing = static_cast<int32>(Snapshot->preparingCount);
	Result.Uploading = static_cast<int32>(Snapshot->uploadingCount);
	Result.Succeeded = static_cast<int32>(Snapshot->succeededCount);
	Result.Failed = static_cast<int32>(Snapshot->failedCount);
	Result.TotalBytes = Snapshot->totalBytes <= static_cast<std::uint64_t>(MAX_int64)
		? static_cast<int64>(Snapshot->totalBytes) : MAX_int64;
	Result.BytesCompleted = Snapshot->bytesCompleted <= static_cast<std::uint64_t>(MAX_int64)
		? static_cast<int64>(Snapshot->bytesCompleted) : MAX_int64;
	Result.ThroughputBytesPerSecond = Snapshot->throughputBytesPerSecond <=
		static_cast<std::uint64_t>(MAX_int64)
		? static_cast<int64>(Snapshot->throughputBytesPerSecond) : MAX_int64;
	Result.EffectiveMaxConcurrentUploads =
		static_cast<int32>(Snapshot->effectiveMaxConcurrentUploads);
	return Result;
}

FDSPGCSUploadJobCommandStatus UDSPGCSUploadJobSubsystem::GetCommandStatus() const
{
	FDSPGCSUploadJobCommandStatus Result;
	Result.bCommandInFlight = bCommandInFlight;
	Result.bCommandTimedOut = bCommandTimedOut;
	Result.bHasCommandAck = bHasCommandAck;
	Result.bHasActiveJobBinding = !ActiveJobID.empty();
	Result.bHasExecutableJob = Mirror.HasAuthoritativeSnapshot();
	Result.bHasAuthoritativeFailure = Mirror.HasAuthoritativeError();
	Result.bRecoveryRequired = Mirror.IsRecoveryRequired();
	Result.CurrentCommandType = static_cast<int32>(CurrentCommandType);
	Result.LastCommandAckStatus = bHasCommandAck
		? static_cast<int32>(LastCommandAckStatus) : 0;
	Result.LastCommandResultCode = static_cast<int32>(LastCommandResultCode);
	Result.ActiveUploadJobID = UploadJobFromUtf8(ActiveJobID);
	Result.LastSafeDiagnostic = LastSafeDiagnostic;
	return Result;
}
