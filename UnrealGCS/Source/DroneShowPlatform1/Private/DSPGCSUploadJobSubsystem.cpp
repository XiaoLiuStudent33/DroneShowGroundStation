#include "DSPGCSUploadJobSubsystem.h"

#include "DSPGCSProductionTelemetryProviderSubsystem.h"

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
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		ProviderRef = GameInstance->GetSubsystem<
			UDSPGCSProductionTelemetryProviderSubsystem>();
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
	Mirror.Reset();
	PendingCommandFrame.clear();
	PendingSnapshotRequestFrame.clear();
	PendingSnapshotRequestID.clear();
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
	dspgcs::UploadJobCommandDTO Command;
	Command.clientCommandId = MakeCanonicalID();
	Command.uploadJobId = RequestedUploadJobID.IsEmpty()
		? MakeCanonicalID() : UploadJobToUtf8(RequestedUploadJobID);
	Command.commandType = dspgcs::UploadJobCommandType::Start;
	Command.requestedMaxConcurrentUploads = RequestedMaxConcurrentUploads > 0
		? static_cast<std::uint32_t>(RequestedMaxConcurrentUploads) : 0;
	Command.artifactIdentity.artifactSetId = UploadJobToUtf8(ArtifactSetID);
	Command.artifactIdentity.fleetSessionId = UploadJobToUtf8(FleetSessionID);
	Command.artifactIdentity.assignmentAuthoritySessionId =
		UploadJobToUtf8(AssignmentAuthoritySessionID);
	Command.artifactIdentity.assignmentRevision = AssignmentRevision > 0
		? static_cast<std::uint64_t>(AssignmentRevision) : 0;
	if (!ParseDigestHex(ArtifactSetDigestHex,
			Command.artifactIdentity.artifactSetDigest) ||
		!ParseDigestHex(AssignmentDigestHex,
			Command.artifactIdentity.assignmentDigest))
	{
		OutError = TEXT("Artifact or Assignment digest is not 64 lowercase hex.");
		return false;
	}
	if (!SendCommand(Command, OutError)) return false;
	ActiveJobID = Command.uploadJobId;
	OutUploadJobID = UploadJobFromUtf8(ActiveJobID);
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
	if (!PendingCommandFrame.empty())
	{
		OutError = TEXT("An Upload Job command is already awaiting ACK.");
		return false;
	}
	std::vector<std::uint8_t> Payload;
	std::string Error;
	if (!dspgcs::EncodeUploadJobCommand(Command, Payload, Error))
	{
		OutError = UploadJobFromUtf8(Error);
		return false;
	}
	std::vector<std::uint8_t> Frame;
	if (!SendPayload(dspgcs::UploadJobMessageTypes::Command,
		Command.clientCommandId, Payload, &Frame, OutError)) return false;
	PendingCommandFrame = std::move(Frame);
	Mirror.SetPendingCommandID(Command.clientCommandId);
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
				FString Ignored;
				RequestAuthoritativeSnapshot(Ignored);
			}
		}
		else if (Frame.Envelope.messageType == dspgcs::UploadJobMessageTypes::Snapshot)
		{
			dspgcs::UploadJobSnapshotDTO Snapshot;
			if (dspgcs::DecodeUploadJobSnapshot(Frame.Payload, Snapshot, Error) &&
				Snapshot.clientRequestId == PendingSnapshotRequestID &&
				Mirror.ApplySnapshot(Snapshot))
			{
				bSnapshotRequestInFlight = false;
				PendingSnapshotRequestFrame.clear();
				PendingSnapshotRequestID.clear();
				PendingSnapshotAttemptCount = 0;
				if (Snapshot.jobState == dspgcs::UploadJobState::Validating)
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
				bSnapshotRequestInFlight = false;
				PendingSnapshotRequestFrame.clear();
				PendingSnapshotRequestID.clear();
				PendingSnapshotAttemptCount = 0;
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
			Mirror.MarkTransportReconnected();
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
			PendingSnapshotRequestFrame.clear();
			PendingSnapshotRequestID.clear();
			PendingSnapshotAttemptCount = 0;
			bSnapshotRequestInFlight = false;
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
