#include "DSPGCSFrozenAssignmentSyncSubsystem.h"

#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "FleetManagerSubsystem.h"
#include "RouteAssignmentSubsystem.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Containers/StringConv.h"
#include "Misc/Guid.h"

#include <limits>
#include <unordered_set>

DEFINE_LOG_CATEGORY_STATIC(LogDSPGCSFrozenAssignmentSync, Log, All);

namespace
{
	bool IsValidStableSourceDigest(const FRouteArtifactDigest256& Digest)
	{
		if (!Digest.IsValid())
		{
			return false;
		}

		for (int32 ByteIndex = 0; ByteIndex < Digest.Num(); ++ByteIndex)
		{
			if (Digest.GetData()[ByteIndex] != 0)
			{
				return true;
			}
		}

		return false;
	}

	std::string ToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value);
		return std::string(Converted.Get(), Converted.Length());
	}

	FString FromUtf8(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	int64 ToUnixMilliseconds(const FDateTime& Value)
	{
		const FDateTime UnixEpoch(1970, 1, 1);
		return (Value - UnixEpoch).GetTicks() / ETimespan::TicksPerMillisecond;
	}

	int64 UtcNowMilliseconds()
	{
		return ToUnixMilliseconds(FDateTime::UtcNow());
	}

	FString ErrorCodeText(const dspgcs::FrozenAssignmentErrorCode ErrorCode)
	{
		switch (ErrorCode)
		{
		case dspgcs::FrozenAssignmentErrorCode::None:
			return TEXT("None");
		case dspgcs::FrozenAssignmentErrorCode::UnsupportedProtocolVersion:
			return TEXT("Unsupported protocol version.");
		case dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch:
			return TEXT("Fleet session mismatch.");
		case dspgcs::FrozenAssignmentErrorCode::MalformedAssignment:
			return TEXT("Malformed frozen assignment.");
		case dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction:
			return TEXT("Incomplete frozen assignment transaction.");
		case dspgcs::FrozenAssignmentErrorCode::HashMismatch:
			return TEXT("Frozen assignment digest mismatch.");
		case dspgcs::FrozenAssignmentErrorCode::DuplicateDroneID:
			return TEXT("Duplicate DroneID.");
		case dspgcs::FrozenAssignmentErrorCode::DuplicateRouteSlot:
			return TEXT("Duplicate RouteSlotIndex.");
		case dspgcs::FrozenAssignmentErrorCode::RouteSlotOutOfRange:
			return TEXT("RouteSlotIndex is outside the route domain.");
		case dspgcs::FrozenAssignmentErrorCode::UnknownDroneID:
			return TEXT("Console rejected an unknown DroneID.");
		case dspgcs::FrozenAssignmentErrorCode::StaleRevision:
			return TEXT("Assignment revision is stale.");
		case dspgcs::FrozenAssignmentErrorCode::RevisionCollision:
			return TEXT("Assignment revision collision.");
		case dspgcs::FrozenAssignmentErrorCode::InternalError:
			return TEXT("Frozen assignment transport failed.");
		case dspgcs::FrozenAssignmentErrorCode::StaleAuthoritySession:
			return TEXT("Assignment authority session is stale.");
		default:
			return TEXT("Frozen assignment sync failed.");
		}
	}

	bool ValidateSnapshotForWire(
		const FRouteAssignmentSnapshot& Snapshot,
		FString& OutError)
	{
		OutError.Reset();
		if (Snapshot.SourceIdentity.FleetSessionID.IsEmpty() ||
			Snapshot.AssignmentAuthoritySessionID.IsEmpty() ||
			Snapshot.AssignmentRevision <= 0)
		{
			OutError = TEXT("Frozen assignment identity is incomplete.");
			return false;
		}
		if (Snapshot.SourceIdentity.FleetRevisionAtCapture < 0 ||
			Snapshot.SourceIdentity.RouteDatasetGeneration <= 0)
		{
			OutError = TEXT("Frozen assignment source revision is invalid.");
			return false;
		}
		if (!IsValidStableSourceDigest(Snapshot.SourceIdentity.SourceDatasetDigest) ||
			Snapshot.SourceIdentity.NormalizedShowOriginLatitudeE7 < -900000000 ||
			Snapshot.SourceIdentity.NormalizedShowOriginLatitudeE7 > 900000000 ||
			Snapshot.SourceIdentity.NormalizedShowOriginLongitudeE7 < -1800000000 ||
			Snapshot.SourceIdentity.NormalizedShowOriginLongitudeE7 > 1800000000)
		{
			OutError = TEXT("Frozen assignment stable source identity is invalid.");
			return false;
		}
		if (Snapshot.CandidateDroneCount < 0 || Snapshot.RouteSlotCount <= 0 ||
			Snapshot.MatchedEntries.IsEmpty())
		{
			OutError = TEXT("Frozen assignment counts are invalid.");
			return false;
		}
		if (!Snapshot.UnresolvedDrones.IsEmpty() || !Snapshot.Conflicts.IsEmpty())
		{
			OutError = TEXT("Frozen assignment contains unresolved or conflicting entries.");
			return false;
		}

		const int64 CandidatePartitionCount =
			static_cast<int64>(Snapshot.MatchedEntries.Num()) +
			Snapshot.SpareDrones.Num() + Snapshot.ExcludedDrones.Num() +
			Snapshot.UnresolvedDrones.Num();
		if (CandidatePartitionCount != Snapshot.CandidateDroneCount ||
			Snapshot.Summary.MatchedCount != Snapshot.MatchedEntries.Num() ||
			Snapshot.Summary.SpareCount != Snapshot.SpareDrones.Num() ||
			Snapshot.Summary.ExcludedCount != Snapshot.ExcludedDrones.Num() ||
			Snapshot.Summary.UnresolvedCount != Snapshot.UnresolvedDrones.Num() ||
			Snapshot.Summary.VacantRouteSlotCount != Snapshot.VacantRouteSlots.Num() ||
			Snapshot.Summary.ConflictCount != Snapshot.Conflicts.Num())
		{
			OutError = TEXT("Frozen assignment partition summary is inconsistent.");
			return false;
		}
		if (static_cast<int64>(Snapshot.MatchedEntries.Num()) +
			Snapshot.VacantRouteSlots.Num() != Snapshot.RouteSlotCount)
		{
			OutError = TEXT("Frozen assignment route partition is incomplete.");
			return false;
		}

		const bool bFull =
			Snapshot.CompletionMode == ERouteAssignmentCompletionMode::Full;
		const bool bDegraded =
			Snapshot.CompletionMode == ERouteAssignmentCompletionMode::Degraded;
		if ((!bFull && !bDegraded) ||
			Snapshot.FreezePolicy == ERouteAssignmentFreezePolicy::Unset ||
			(bFull && !Snapshot.VacantRouteSlots.IsEmpty()) ||
			(bDegraded && (Snapshot.VacantRouteSlots.IsEmpty() ||
				Snapshot.FreezePolicy != ERouteAssignmentFreezePolicy::AllowDegraded)) ||
			(bFull && Snapshot.ReadinessKind != ERouteAssignmentReadinessKind::FullReady) ||
			(bDegraded && Snapshot.ReadinessKind != ERouteAssignmentReadinessKind::DegradedReady))
		{
			OutError = TEXT("Frozen assignment completion or freeze policy is inconsistent.");
			return false;
		}

		std::unordered_set<std::string> DroneIDs;
		std::unordered_set<int32> RouteSlots;
		DroneIDs.reserve(Snapshot.MatchedEntries.Num());
		RouteSlots.reserve(Snapshot.RouteSlotCount);
		for (const FRouteAssignmentEntry& Entry : Snapshot.MatchedEntries)
		{
			const std::string DroneID = ToUtf8(Entry.DroneID);
			if (DroneID.empty() || Entry.RouteSlotIndex < 0 ||
				Entry.RouteSlotIndex >= Snapshot.RouteSlotCount ||
				!DroneIDs.insert(DroneID).second ||
				!RouteSlots.insert(Entry.RouteSlotIndex).second)
			{
				OutError = TEXT("Frozen assignment mapping identity or RouteSlotIndex is invalid.");
				return false;
			}
		}
		for (const int32 RouteSlotIndex : Snapshot.VacantRouteSlots)
		{
			if (RouteSlotIndex < 0 || RouteSlotIndex >= Snapshot.RouteSlotCount ||
				!RouteSlots.insert(RouteSlotIndex).second)
			{
				OutError = TEXT("Frozen assignment vacant route partition is invalid.");
				return false;
			}
		}
		if (RouteSlots.size() != static_cast<std::size_t>(Snapshot.RouteSlotCount) ||
			Snapshot.FrozenAtUtc.GetTicks() <= FDateTime(1970, 1, 1).GetTicks())
		{
			OutError = TEXT("Frozen assignment route partition or timestamp is invalid.");
			return false;
		}
		return true;
	}

	dspgcs::MessageEnvelope MakeEnvelope(
		const dspgcs::MessageType MessageType,
		const FDSPGCSFrozenAssignmentWireTransaction& Transaction,
		const std::uint64_t Sequence,
		const std::int64_t TimestampUtcMs,
		const std::uint64_t FleetRevision)
	{
		dspgcs::MessageEnvelope Envelope;
		Envelope.messageType = MessageType;
		Envelope.sourceId = "UnrealGCS-FrozenAssignment";
		Envelope.sessionId = Transaction.Identity.fleetSessionId;
		Envelope.sequence = Sequence;
		Envelope.timestampUtcMs = TimestampUtcMs;
		Envelope.correlationId = ToUtf8(Transaction.TransactionID);
		Envelope.fleetRevision = FleetRevision;
		return Envelope;
	}
}

void UDSPGCSFrozenAssignmentSyncSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<URouteAssignmentSubsystem>();
	Collection.InitializeDependency<UFleetManagerSubsystem>();
	Collection.InitializeDependency<UDSPGCSProductionTelemetryProviderSubsystem>();

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		RouteAssignmentRef = GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
		FleetManagerRef = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
		TransportProviderRef =
			GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>();
	}

	if (UDSPGCSProductionTelemetryProviderSubsystem* Provider =
		TransportProviderRef.Get())
	{
		const TWeakObjectPtr<UDSPGCSFrozenAssignmentSyncSubsystem> WeakThis(this);
		Provider->SetFrozenAssignmentAckHandler(
			[WeakThis](
				const dspgcs::MessageEnvelope& Envelope,
				const dspgcs::FrozenAssignmentAckDTO& Ack)
			{
				if (UDSPGCSFrozenAssignmentSyncSubsystem* Self = WeakThis.Get())
				{
					Self->EnqueueAck(Envelope, Ack);
				}
			});
	}

	SyncTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ThisClass::TickSync),
		SyncTickIntervalSeconds);
	RefreshFrozenBinding();
}

void UDSPGCSFrozenAssignmentSyncSubsystem::Deinitialize()
{
	if (SyncTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SyncTickerHandle);
		SyncTickerHandle.Reset();
	}
	if (UDSPGCSProductionTelemetryProviderSubsystem* Provider =
		TransportProviderRef.Get())
	{
		Provider->SetFrozenAssignmentAckHandler({});
	}
	TransportProviderRef.Reset();
	FleetManagerRef.Reset();
	RouteAssignmentRef.Reset();
	ResetToNotFrozen();
	Super::Deinitialize();
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::StartSyncFrozenAssignment(
	FString& OutError)
{
	check(IsInGameThread());
	ProcessPendingAcks();
	RefreshFrozenBinding();
	if (Status.State == EDSPGCSFrozenAssignmentSyncState::SyncingToConsole)
	{
		OutError = TEXT("A Frozen Assignment sync is already active.");
		return false;
	}

	FRouteAssignmentSnapshot Snapshot;
	URouteAssignmentSubsystem* Authority = RouteAssignmentRef.Get();
	if (!Authority || !Authority->GetFrozenSnapshot(Snapshot))
	{
		ResetToNotFrozen();
		OutError = TEXT("No legal Frozen Assignment snapshot is available.");
		return false;
	}
	const FString CurrentFleetSessionID = FleetManagerRef.IsValid()
		? FleetManagerRef->GetActiveFleetSessionID()
		: FString();
	return StartFromSnapshot(Snapshot, CurrentFleetSessionID, false, OutError);
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::RetryFailedSync(FString& OutError)
{
	check(IsInGameThread());
	ProcessPendingAcks();
	if (Status.State != EDSPGCSFrozenAssignmentSyncState::SyncFailed)
	{
		OutError = TEXT("Manual retry requires a failed Frozen Assignment sync.");
		return false;
	}

	FRouteAssignmentSnapshot Snapshot;
	URouteAssignmentSubsystem* Authority = RouteAssignmentRef.Get();
	if (!Authority || !Authority->GetFrozenSnapshot(Snapshot))
	{
		ResetToNotFrozen();
		OutError = TEXT("The failed Frozen Assignment is no longer frozen.");
		return false;
	}
	const FString CurrentFleetSessionID = FleetManagerRef.IsValid()
		? FleetManagerRef->GetActiveFleetSessionID()
		: FString();
	return StartFromSnapshot(Snapshot, CurrentFleetSessionID, true, OutError);
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::CancelActiveSync(FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (Status.State != EDSPGCSFrozenAssignmentSyncState::SyncingToConsole ||
		!bHasActiveTransaction)
	{
		OutError = TEXT("No Frozen Assignment sync is active.");
		return false;
	}

	dspgcs::FrozenAssignmentAbortDTO Abort;
	Abort.transactionId = ToUtf8(ActiveTransaction.TransactionID);
	std::vector<std::uint8_t> Payload;
	std::string EncodeError;
	if (dspgcs::EncodeFrozenAssignmentAbort(Abort, Payload, EncodeError))
	{
		const dspgcs::MessageEnvelope Envelope = MakeEnvelope(
			dspgcs::FrozenAssignmentMessageTypes::Abort,
			ActiveTransaction,
			++OutgoingSequence,
			UtcNowMilliseconds(),
			0);
		const std::vector<std::uint8_t> Frame = dspgcs::MakeFrame(Envelope, Payload);
		if (Frame.size() <= dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes)
		{
			SendFrame(Frame);
		}
	}

	BindStatusToTransaction(
		EDSPGCSFrozenAssignmentSyncState::FrozenLocal,
		ActiveTransaction);
	Status.TransactionID.Reset();
	Status.AttemptCount = 0;
	bHasActiveTransaction = false;
	bWaitingForAck = false;
	bRetryScheduled = false;
	return true;
}

void UDSPGCSFrozenAssignmentSyncSubsystem::ProcessPendingAcks()
{
	check(IsInGameThread());
	FDSPGCSFrozenAssignmentPendingAck Pending;
	while (PendingAcks.Dequeue(Pending))
	{
		HandleDecodedAck(
			Pending.Envelope,
			Pending.Ack,
			FPlatformTime::Seconds());
	}
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::BuildWireTransaction(
	const FRouteAssignmentSnapshot& Snapshot,
	const FString& TransactionID,
	std::uint64_t& InOutSequence,
	FDSPGCSFrozenAssignmentWireTransaction& OutTransaction,
	FString& OutError,
	const std::size_t DatagramBudget)
{
	OutTransaction = {};
	if (!ValidateSnapshotForWire(Snapshot, OutError))
	{
		return false;
	}
	if (TransactionID.IsEmpty())
	{
		OutError = TEXT("TransactionID is empty.");
		return false;
	}
	if (Snapshot.MatchedEntries.Num() >
		static_cast<int64>(std::numeric_limits<std::uint32_t>::max()))
	{
		OutError = TEXT("Frozen assignment mapping count exceeds the wire domain.");
		return false;
	}

	OutTransaction.TransactionID = TransactionID;
	OutTransaction.Identity.fleetSessionId =
		ToUtf8(Snapshot.SourceIdentity.FleetSessionID);
	OutTransaction.Identity.assignmentAuthoritySessionId =
		ToUtf8(Snapshot.AssignmentAuthoritySessionID);
	OutTransaction.Identity.assignmentRevision =
		static_cast<std::uint64_t>(Snapshot.AssignmentRevision);
	OutTransaction.MatchedCount =
		static_cast<std::uint32_t>(Snapshot.MatchedEntries.Num());

	dspgcs::FrozenAssignmentPayloadDTO WirePayload;
	FMemory::Memcpy(
		WirePayload.sourceDatasetDigest.data(),
		Snapshot.SourceIdentity.SourceDatasetDigest.GetData(),
		FRouteArtifactDigest256::ByteCount);
	WirePayload.normalizedShowOriginLatitudeE7 =
		Snapshot.SourceIdentity.NormalizedShowOriginLatitudeE7;
	WirePayload.normalizedShowOriginLongitudeE7 =
		Snapshot.SourceIdentity.NormalizedShowOriginLongitudeE7;
	WirePayload.routeSlotCount = static_cast<std::uint32_t>(Snapshot.RouteSlotCount);
	WirePayload.identity = OutTransaction.Identity;
	WirePayload.fleetRevisionAtCapture = static_cast<std::uint64_t>(
		Snapshot.SourceIdentity.FleetRevisionAtCapture);
	WirePayload.routeDatasetGeneration = static_cast<std::uint64_t>(
		Snapshot.SourceIdentity.RouteDatasetGeneration);
	WirePayload.completionMode =
		Snapshot.CompletionMode == ERouteAssignmentCompletionMode::Full
			? dspgcs::FrozenAssignmentCompletionMode::Full
			: dspgcs::FrozenAssignmentCompletionMode::Degraded;
	WirePayload.freezePolicy =
		Snapshot.FreezePolicy == ERouteAssignmentFreezePolicy::RequireFull
			? dspgcs::FrozenAssignmentFreezePolicy::RequireFull
			: dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
	WirePayload.matchedCount = OutTransaction.MatchedCount;
	WirePayload.spareCount = static_cast<std::uint32_t>(Snapshot.SpareDrones.Num());
	WirePayload.excludedCount = static_cast<std::uint32_t>(Snapshot.ExcludedDrones.Num());
	WirePayload.vacantCount = static_cast<std::uint32_t>(Snapshot.VacantRouteSlots.Num());
	WirePayload.frozenAtUtcMs = ToUnixMilliseconds(Snapshot.FrozenAtUtc);
	WirePayload.mappings.reserve(Snapshot.MatchedEntries.Num());
	for (const FRouteAssignmentEntry& Entry : Snapshot.MatchedEntries)
	{
		dspgcs::FrozenAssignmentMappingDTO Mapping;
		Mapping.droneId = ToUtf8(Entry.DroneID);
		Mapping.routeSlotIndex = Entry.RouteSlotIndex;
		WirePayload.mappings.push_back(MoveTemp(Mapping));
	}

	std::vector<std::uint8_t> CanonicalPayload;
	std::string WireError;
	if (!dspgcs::SerializeCanonicalFrozenAssignmentPayload(
		WirePayload, CanonicalPayload, WireError) ||
		!dspgcs::ComputeFrozenAssignmentSha256(
			CanonicalPayload, OutTransaction.Digest, WireError))
	{
		OutError = FromUtf8(WireError);
		return false;
	}
	OutTransaction.CanonicalPayloadBytes = CanonicalPayload.size();

	const std::int64_t TimestampUtcMs = UtcNowMilliseconds();
	const dspgcs::MessageEnvelope PlanEnvelope = MakeEnvelope(
		dspgcs::FrozenAssignmentMessageTypes::Chunk,
		OutTransaction,
		InOutSequence + 2,
		TimestampUtcMs,
		WirePayload.fleetRevisionAtCapture);
	dspgcs::FrozenAssignmentChunkPlan ChunkPlan;
	if (!dspgcs::PlanFrozenAssignmentChunks(
		CanonicalPayload,
		DatagramBudget,
		PlanEnvelope,
		ToUtf8(TransactionID),
		ChunkPlan,
		WireError))
	{
		OutError = FromUtf8(WireError);
		return false;
	}
	OutTransaction.ChunkCount =
		static_cast<std::uint32_t>(ChunkPlan.chunks.size());

	dspgcs::FrozenAssignmentBeginDTO Begin;
	Begin.transactionId = ToUtf8(TransactionID);
	Begin.sourceDatasetDigest = WirePayload.sourceDatasetDigest;
	Begin.normalizedShowOriginLatitudeE7 =
		WirePayload.normalizedShowOriginLatitudeE7;
	Begin.normalizedShowOriginLongitudeE7 =
		WirePayload.normalizedShowOriginLongitudeE7;
	Begin.routeSlotCount = WirePayload.routeSlotCount;
	Begin.identity = WirePayload.identity;
	Begin.fleetRevisionAtCapture = WirePayload.fleetRevisionAtCapture;
	Begin.routeDatasetGeneration = WirePayload.routeDatasetGeneration;
	Begin.completionMode = WirePayload.completionMode;
	Begin.freezePolicy = WirePayload.freezePolicy;
	Begin.matchedCount = WirePayload.matchedCount;
	Begin.spareCount = WirePayload.spareCount;
	Begin.excludedCount = WirePayload.excludedCount;
	Begin.vacantCount = WirePayload.vacantCount;
	Begin.frozenAtUtcMs = WirePayload.frozenAtUtcMs;
	Begin.totalPayloadBytes = CanonicalPayload.size();
	Begin.chunkCount = OutTransaction.ChunkCount;
	Begin.canonicalPayloadDigest = OutTransaction.Digest;

	std::vector<std::uint8_t> EncodedPayload;
	if (!dspgcs::EncodeFrozenAssignmentBegin(Begin, EncodedPayload, WireError))
	{
		OutError = FromUtf8(WireError);
		return false;
	}
	OutTransaction.Frames.push_back(dspgcs::MakeFrame(
		MakeEnvelope(
			dspgcs::FrozenAssignmentMessageTypes::Begin,
			OutTransaction,
			++InOutSequence,
			TimestampUtcMs,
			WirePayload.fleetRevisionAtCapture),
		EncodedPayload));

	for (std::size_t Index = 0; Index < ChunkPlan.chunks.size(); ++Index)
	{
		dspgcs::FrozenAssignmentChunkDTO Chunk;
		if (!dspgcs::BuildFrozenAssignmentChunk(
			CanonicalPayload, ChunkPlan, Index, ToUtf8(TransactionID), Chunk, WireError) ||
			!dspgcs::EncodeFrozenAssignmentChunk(Chunk, EncodedPayload, WireError))
		{
			OutError = FromUtf8(WireError);
			return false;
		}
		OutTransaction.Frames.push_back(dspgcs::MakeFrame(
			MakeEnvelope(
				dspgcs::FrozenAssignmentMessageTypes::Chunk,
				OutTransaction,
				++InOutSequence,
				TimestampUtcMs,
				WirePayload.fleetRevisionAtCapture),
			EncodedPayload));
	}

	dspgcs::FrozenAssignmentCommitDTO Commit;
	Commit.transactionId = ToUtf8(TransactionID);
	Commit.identity = WirePayload.identity;
	Commit.canonicalPayloadDigest = OutTransaction.Digest;
	if (!dspgcs::EncodeFrozenAssignmentCommit(Commit, EncodedPayload, WireError))
	{
		OutError = FromUtf8(WireError);
		return false;
	}
	OutTransaction.Frames.push_back(dspgcs::MakeFrame(
		MakeEnvelope(
			dspgcs::FrozenAssignmentMessageTypes::Commit,
			OutTransaction,
			++InOutSequence,
			TimestampUtcMs,
			WirePayload.fleetRevisionAtCapture),
		EncodedPayload));

	for (const std::vector<std::uint8_t>& Frame : OutTransaction.Frames)
	{
		if (Frame.empty() || Frame.size() > DatagramBudget)
		{
			OutError = TEXT("An encoded DSP Frozen Assignment frame exceeds the datagram budget.");
			OutTransaction = {};
			return false;
		}
		OutTransaction.EncodedTotalBytes += Frame.size();
	}
	return true;
}

FString UDSPGCSFrozenAssignmentSyncSubsystem::DigestToHex(
	const dspgcs::FrozenAssignmentDigest& Digest)
{
	static constexpr TCHAR Hex[] = TEXT("0123456789abcdef");
	FString Result;
	Result.Reserve(static_cast<int32>(Digest.size() * 2));
	for (const std::uint8_t Byte : Digest)
	{
		Result.AppendChar(Hex[(Byte >> 4) & 0x0f]);
		Result.AppendChar(Hex[Byte & 0x0f]);
	}
	return Result;
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::IsRetryableError(
	const dspgcs::FrozenAssignmentErrorCode ErrorCode)
{
	return ErrorCode == dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction ||
		ErrorCode == dspgcs::FrozenAssignmentErrorCode::HashMismatch;
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::IsSameBinding(
	const FDSPGCSFrozenAssignmentSyncStatus& StatusToCompare,
	const FDSPGCSFrozenAssignmentWireTransaction& Transaction)
{
	return StatusToCompare.FleetSessionID ==
			FromUtf8(Transaction.Identity.fleetSessionId) &&
		StatusToCompare.AssignmentAuthoritySessionID ==
			FromUtf8(Transaction.Identity.assignmentAuthoritySessionId) &&
		StatusToCompare.AssignmentRevision ==
			static_cast<int64>(Transaction.Identity.assignmentRevision) &&
		StatusToCompare.CanonicalPayloadDigest == DigestToHex(Transaction.Digest);
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::StartFromSnapshot(
	const FRouteAssignmentSnapshot& Snapshot,
	const FString& CurrentFleetSessionID,
	const bool bRequireFailedSameAssignment,
	FString& OutError)
{
	OutError.Reset();
	FDSPGCSFrozenAssignmentWireTransaction NewTransaction;
	std::uint64_t CandidateSequence = OutgoingSequence;
	if (!BuildWireTransaction(
		Snapshot,
		MakeTransactionID(),
		CandidateSequence,
		NewTransaction,
		OutError))
	{
		Status.State = EDSPGCSFrozenAssignmentSyncState::SyncFailed;
		Status.LastErrorCode = static_cast<int32>(
			dspgcs::FrozenAssignmentErrorCode::MalformedAssignment);
		Status.LastError = OutError;
		return false;
	}

	if (bRequireFailedSameAssignment &&
		(Status.State != EDSPGCSFrozenAssignmentSyncState::SyncFailed ||
			!IsSameBinding(Status, NewTransaction)))
	{
		OutError = TEXT("The Frozen Assignment changed after the failed sync.");
		return false;
	}
	if (Status.State == EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted &&
		IsSameBinding(Status, NewTransaction))
	{
		OutError = TEXT("The current Frozen Assignment is already Console-accepted.");
		return false;
	}

	OutgoingSequence = CandidateSequence;
	ActiveTransaction = MoveTemp(NewTransaction);
	bHasActiveTransaction = true;
	BindStatusToTransaction(
		EDSPGCSFrozenAssignmentSyncState::SyncingToConsole,
		ActiveTransaction);
	if (!CurrentFleetSessionID.IsEmpty() &&
		CurrentFleetSessionID != Status.FleetSessionID)
	{
		OutError = ErrorCodeText(
			dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch);
		FailSync(
			dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch,
			OutError);
		return false;
	}

	UE_LOG(
		LogDSPGCSFrozenAssignmentSync,
		Display,
		TEXT("Frozen Assignment sync start Transaction=%s Revision=%lld Matched=%d Chunks=%d Digest=%s"),
		*Status.TransactionID,
		Status.AssignmentRevision,
		Status.MatchedCount,
		Status.ChunkCount,
		*Status.CanonicalPayloadDigest.Left(12));
	SendActiveAttempt(FPlatformTime::Seconds());
	return true;
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::SendActiveAttempt(
	const double CurrentTimeSeconds)
{
	if (!bHasActiveTransaction ||
		Status.State != EDSPGCSFrozenAssignmentSyncState::SyncingToConsole)
	{
		return false;
	}
	++Status.AttemptCount;
	bWaitingForAck = false;
	bRetryScheduled = false;
	for (const std::vector<std::uint8_t>& Frame : ActiveTransaction.Frames)
	{
		if (!SendFrame(Frame))
		{
			if (Status.AttemptCount >= MaxAttempts)
			{
				FailSync(
					dspgcs::FrozenAssignmentErrorCode::InternalError,
					ErrorCodeText(dspgcs::FrozenAssignmentErrorCode::InternalError));
			}
			else
			{
				bRetryScheduled = true;
				RetryAtSeconds = CurrentTimeSeconds +
					InitialRetryDelaySeconds *
					static_cast<double>(1 << (Status.AttemptCount - 1));
			}
			return false;
		}
	}
	bWaitingForAck = true;
	AckDeadlineSeconds = CurrentTimeSeconds + AckTimeoutSeconds;
	UE_LOG(
		LogDSPGCSFrozenAssignmentSync,
		Display,
		TEXT("Frozen Assignment transaction sent Transaction=%s Attempt=%d Chunks=%d"),
		*Status.TransactionID,
		Status.AttemptCount,
		Status.ChunkCount);
	return true;
}

void UDSPGCSFrozenAssignmentSyncSubsystem::ScheduleRetryOrFail(
	const dspgcs::FrozenAssignmentErrorCode ErrorCode,
	const FString& SafeDiagnostic,
	const double CurrentTimeSeconds)
{
	bWaitingForAck = false;
	if (Status.AttemptCount >= MaxAttempts)
	{
		FailSync(ErrorCode, SafeDiagnostic);
		return;
	}
	bRetryScheduled = true;
	RetryAtSeconds = CurrentTimeSeconds + InitialRetryDelaySeconds *
		static_cast<double>(1 << (Status.AttemptCount - 1));
	Status.LastErrorCode = static_cast<int32>(ErrorCode);
	Status.LastError = SafeDiagnostic;
}

void UDSPGCSFrozenAssignmentSyncSubsystem::FailSync(
	const dspgcs::FrozenAssignmentErrorCode ErrorCode,
	const FString& SafeDiagnostic)
{
	Status.State = EDSPGCSFrozenAssignmentSyncState::SyncFailed;
	Status.LastErrorCode = static_cast<int32>(ErrorCode);
	Status.LastError = SafeDiagnostic;
	bHasActiveTransaction = false;
	bWaitingForAck = false;
	bRetryScheduled = false;
	UE_LOG(
		LogDSPGCSFrozenAssignmentSync,
		Warning,
		TEXT("Frozen Assignment sync failed Transaction=%s Attempt=%d ErrorCode=%d"),
		*Status.TransactionID,
		Status.AttemptCount,
		Status.LastErrorCode);
}

void UDSPGCSFrozenAssignmentSyncSubsystem::HandleDecodedAck(
	const dspgcs::MessageEnvelope& Envelope,
	const dspgcs::FrozenAssignmentAckDTO& Ack,
	const double CurrentTimeSeconds)
{
	if (!bHasActiveTransaction ||
		Status.State != EDSPGCSFrozenAssignmentSyncState::SyncingToConsole)
	{
		return;
	}
	const std::string TransactionID = ToUtf8(ActiveTransaction.TransactionID);
	if (Envelope.messageType != dspgcs::FrozenAssignmentMessageTypes::Ack ||
		Envelope.correlationId != TransactionID || Ack.transactionId != TransactionID ||
		Ack.identity.fleetSessionId != ActiveTransaction.Identity.fleetSessionId ||
		Ack.identity.assignmentAuthoritySessionId !=
			ActiveTransaction.Identity.assignmentAuthoritySessionId ||
		Ack.identity.assignmentRevision != ActiveTransaction.Identity.assignmentRevision)
	{
		return;
	}

	if (Ack.result == dspgcs::FrozenAssignmentAckResult::Accepted)
	{
		if (Ack.errorCode != dspgcs::FrozenAssignmentErrorCode::None ||
			Ack.acceptedDigest != ActiveTransaction.Digest ||
			Ack.acceptedMatchedCount != ActiveTransaction.MatchedCount)
		{
			return;
		}
		Status.State = EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted;
		Status.LastErrorCode = 0;
		Status.LastError.Reset();
		Status.AcceptedAtUtc = FDateTime::UtcNow();
		bHasActiveTransaction = false;
		bWaitingForAck = false;
		bRetryScheduled = false;
		UE_LOG(
			LogDSPGCSFrozenAssignmentSync,
			Display,
			TEXT("Frozen Assignment accepted Transaction=%s Revision=%lld Matched=%d Digest=%s"),
			*Status.TransactionID,
			Status.AssignmentRevision,
			Status.MatchedCount,
			*Status.CanonicalPayloadDigest.Left(12));
		return;
	}

	const FString SafeDiagnostic = ErrorCodeText(Ack.errorCode);
	if (IsRetryableError(Ack.errorCode))
	{
		ScheduleRetryOrFail(Ack.errorCode, SafeDiagnostic, CurrentTimeSeconds);
	}
	else
	{
		FailSync(Ack.errorCode, SafeDiagnostic);
	}
}

void UDSPGCSFrozenAssignmentSyncSubsystem::EnqueueAck(
	const dspgcs::MessageEnvelope& Envelope,
	const dspgcs::FrozenAssignmentAckDTO& Ack)
{
	if (Envelope.messageType != dspgcs::FrozenAssignmentMessageTypes::Ack)
	{
		return;
	}
	FDSPGCSFrozenAssignmentPendingAck Pending;
	Pending.Envelope = Envelope;
	Pending.Ack = Ack;
	PendingAcks.Enqueue(MoveTemp(Pending));
}

void UDSPGCSFrozenAssignmentSyncSubsystem::RefreshFrozenBinding()
{
	check(IsInGameThread());
	FRouteAssignmentSnapshot Snapshot;
	URouteAssignmentSubsystem* Authority = RouteAssignmentRef.Get();
	const bool bHasFrozen = Authority && Authority->GetFrozenSnapshot(Snapshot);
	const FString CurrentFleetSessionID = FleetManagerRef.IsValid()
		? FleetManagerRef->GetActiveFleetSessionID()
		: FString();
	RefreshFromSnapshot(bHasFrozen ? &Snapshot : nullptr, CurrentFleetSessionID);
}

void UDSPGCSFrozenAssignmentSyncSubsystem::RefreshFromSnapshot(
	const FRouteAssignmentSnapshot* Snapshot,
	const FString& CurrentFleetSessionID)
{
	if (!Snapshot)
	{
		ResetToNotFrozen();
		return;
	}

	FDSPGCSFrozenAssignmentWireTransaction Probe;
	std::uint64_t ProbeSequence = OutgoingSequence;
	FString Error;
	if (!BuildWireTransaction(
		*Snapshot,
		TEXT("binding-probe"),
		ProbeSequence,
		Probe,
		Error))
	{
		ResetToNotFrozen();
		Status.LastErrorCode = static_cast<int32>(
			dspgcs::FrozenAssignmentErrorCode::MalformedAssignment);
		Status.LastError = Error;
		return;
	}

	const bool bFleetMatches = CurrentFleetSessionID.IsEmpty() ||
		CurrentFleetSessionID == FromUtf8(Probe.Identity.fleetSessionId);
	if (bFleetMatches && IsSameBinding(Status, Probe))
	{
		return;
	}

	BindStatusToTransaction(
		EDSPGCSFrozenAssignmentSyncState::FrozenLocal,
		Probe);
	bHasActiveTransaction = false;
	bWaitingForAck = false;
	bRetryScheduled = false;
}

void UDSPGCSFrozenAssignmentSyncSubsystem::AdvanceRetryStateAt(
	const double CurrentTimeSeconds)
{
	if (Status.State != EDSPGCSFrozenAssignmentSyncState::SyncingToConsole ||
		!bHasActiveTransaction)
	{
		return;
	}
	if (bWaitingForAck && CurrentTimeSeconds >= AckDeadlineSeconds)
	{
		ScheduleRetryOrFail(
			dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
			TEXT("Frozen Assignment ACK timed out."),
			CurrentTimeSeconds);
	}
	if (bRetryScheduled && CurrentTimeSeconds >= RetryAtSeconds)
	{
		SendActiveAttempt(CurrentTimeSeconds);
	}
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::TickSync(float DeltaSeconds)
{
	(void)DeltaSeconds;
	ProcessPendingAcks();
	const double CurrentTimeSeconds = FPlatformTime::Seconds();
	AdvanceRetryStateAt(CurrentTimeSeconds);
	if (CurrentTimeSeconds >= NextBindingRefreshSeconds)
	{
		RefreshFrozenBinding();
		NextBindingRefreshSeconds =
			CurrentTimeSeconds + BindingRefreshIntervalSeconds;
	}
	return true;
}

bool UDSPGCSFrozenAssignmentSyncSubsystem::SendFrame(
	const std::vector<std::uint8_t>& Frame)
{
	if (TestFrameSender)
	{
		return TestFrameSender(Frame);
	}
	UDSPGCSProductionTelemetryProviderSubsystem* Provider =
		TransportProviderRef.Get();
	return Provider && Provider->SendFrame(Frame);
}

FString UDSPGCSFrozenAssignmentSyncSubsystem::MakeTransactionID() const
{
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

void UDSPGCSFrozenAssignmentSyncSubsystem::BindStatusToTransaction(
	const EDSPGCSFrozenAssignmentSyncState NewState,
	const FDSPGCSFrozenAssignmentWireTransaction& Transaction)
{
	Status.State = NewState;
	Status.FleetSessionID = FromUtf8(Transaction.Identity.fleetSessionId);
	Status.AssignmentAuthoritySessionID =
		FromUtf8(Transaction.Identity.assignmentAuthoritySessionId);
	Status.AssignmentRevision =
		static_cast<int64>(Transaction.Identity.assignmentRevision);
	Status.CanonicalPayloadDigest = DigestToHex(Transaction.Digest);
	Status.TransactionID = NewState == EDSPGCSFrozenAssignmentSyncState::FrozenLocal
		? FString()
		: Transaction.TransactionID;
	Status.AttemptCount = 0;
	Status.MatchedCount = static_cast<int32>(Transaction.MatchedCount);
	Status.ChunkCount = static_cast<int32>(Transaction.ChunkCount);
	Status.LastErrorCode = 0;
	Status.LastError.Reset();
	Status.AcceptedAtUtc = FDateTime();
}

void UDSPGCSFrozenAssignmentSyncSubsystem::ResetToNotFrozen()
{
	Status = {};
	ActiveTransaction = {};
	bHasActiveTransaction = false;
	bWaitingForAck = false;
	bRetryScheduled = false;
}
