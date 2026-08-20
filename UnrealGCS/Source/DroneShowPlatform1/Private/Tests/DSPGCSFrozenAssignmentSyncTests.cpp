#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Algo/Reverse.h"
#include "Async/Async.h"
#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "DSPGCSTransportConfig.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RouteAssignmentSubsystem.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Async/TaskGraphInterfaces.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace
{
	FRouteAssignmentSnapshot MakeFrozenSnapshot(
		const int32 MatchedCount,
		const bool bDegraded = false,
		const int32 SpareCount = 0,
		const int32 ExcludedCount = 0)
	{
		FRouteAssignmentSnapshot Snapshot;
		Snapshot.AssignmentAuthoritySessionID = TEXT("AUTHORITY-R2");
		Snapshot.AssignmentRevision = 5;
		Snapshot.SourceIdentity.FleetSessionID = TEXT("FLEET-R2");
		uint8 SourceDigestBytes[FRouteArtifactDigest256::ByteCount];
		FMemory::Memset(SourceDigestBytes, 0x5a, sizeof(SourceDigestBytes));
		FString SourceDigestError;
		check(FRouteArtifactDigest256::FromBytes(
			SourceDigestBytes,
			UE_ARRAY_COUNT(SourceDigestBytes),
			Snapshot.SourceIdentity.SourceDatasetDigest,
			SourceDigestError));
		Snapshot.SourceIdentity.NormalizedShowOriginLatitudeE7 = 301230000;
		Snapshot.SourceIdentity.NormalizedShowOriginLongitudeE7 = 1204560000;
		Snapshot.SourceIdentity.FleetRevisionAtCapture = 17;
		Snapshot.SourceIdentity.RouteDatasetGeneration = 9;
		Snapshot.SourceIdentity.bShowOriginSet = true;
		Snapshot.SourceIdentity.Latitude = 30.123;
		Snapshot.SourceIdentity.Longitude = 120.456;
		Snapshot.SourceIdentity.Altitude = 42.0;
		Snapshot.SourceIdentity.CapturedAtUtc = FDateTime(2026, 8, 16, 23, 59, 0);
		Snapshot.CandidateDroneCount = MatchedCount + SpareCount + ExcludedCount;
		Snapshot.RouteSlotCount = MatchedCount + (bDegraded ? 1 : 0);
		Snapshot.MatchedEntries.Reserve(MatchedCount);
		for (int32 Index = 0; Index < MatchedCount; ++Index)
		{
			FRouteAssignmentEntry Entry;
			Entry.DroneID = FString::Printf(TEXT("Drone%06d"), Index);
			Entry.RouteSlotIndex = Index;
			Entry.HorizontalDistanceMeters = 1000.0 + Index * 0.125;
			Entry.bHorizontalDistanceValid = true;
			Snapshot.MatchedEntries.Add(MoveTemp(Entry));
		}
		for (int32 Index = 0; Index < SpareCount; ++Index)
		{
			FRouteAssignmentSpareDrone Spare;
			Spare.DroneID = FString::Printf(TEXT("Spare%06d"), Index);
			Snapshot.SpareDrones.Add(MoveTemp(Spare));
		}
		for (int32 Index = 0; Index < ExcludedCount; ++Index)
		{
			FRouteAssignmentExcludedDrone Excluded;
			Excluded.DroneID = FString::Printf(TEXT("Excluded%06d"), Index);
			Excluded.Reason = ERouteAssignmentExcludedReason::Offline;
			Snapshot.ExcludedDrones.Add(MoveTemp(Excluded));
		}
		if (bDegraded)
		{
			Snapshot.VacantRouteSlots.Add(MatchedCount);
		}
		Snapshot.Summary.MatchedCount = MatchedCount;
		Snapshot.Summary.SpareCount = SpareCount;
		Snapshot.Summary.ExcludedCount = ExcludedCount;
		Snapshot.Summary.VacantRouteSlotCount = bDegraded ? 1 : 0;
		Snapshot.ReadinessKind = bDegraded
			? ERouteAssignmentReadinessKind::DegradedReady
			: ERouteAssignmentReadinessKind::FullReady;
		Snapshot.CompletionMode = bDegraded
			? ERouteAssignmentCompletionMode::Degraded
			: ERouteAssignmentCompletionMode::Full;
		Snapshot.FreezePolicy = bDegraded
			? ERouteAssignmentFreezePolicy::AllowDegraded
			: ERouteAssignmentFreezePolicy::RequireFull;
		Snapshot.FrozenAtUtc = FDateTime(2026, 8, 17, 0, 0, 0, 125);
		return Snapshot;
	}

	FRouteAssignmentCandidate ToCandidate(const FRouteAssignmentSnapshot& Snapshot)
	{
		FRouteAssignmentCandidate Candidate;
		Candidate.BindingProvenance = Snapshot.BindingProvenance;
		Candidate.SourceIdentity = Snapshot.SourceIdentity;
		Candidate.CandidateDroneCount = Snapshot.CandidateDroneCount;
		Candidate.RouteSlotCount = Snapshot.RouteSlotCount;
		Candidate.MatchedEntries = Snapshot.MatchedEntries;
		for (FRouteAssignmentEntry& Entry : Candidate.MatchedEntries)
		{
			Entry.HorizontalDistanceMeters =
				Candidate.BindingProvenance
					== ERouteAssignmentBindingProvenance::Explicit ? 0.0 : 0.1;
			Entry.bHorizontalDistanceValid =
				Candidate.BindingProvenance
					== ERouteAssignmentBindingProvenance::MatchedByPosition;
		}
		Candidate.SpareDrones = Snapshot.SpareDrones;
		Candidate.ExcludedDrones = Snapshot.ExcludedDrones;
		Candidate.UnresolvedDrones = Snapshot.UnresolvedDrones;
		Candidate.VacantRouteSlots = Snapshot.VacantRouteSlots;
		Candidate.Conflicts = Snapshot.Conflicts;
		return Candidate;
	}

	struct FDecodedWireTransaction
	{
		dspgcs::FrozenAssignmentBeginDTO Begin;
		dspgcs::FrozenAssignmentCommitDTO Commit;
		dspgcs::FrozenAssignmentPayloadDTO Payload;
		std::vector<std::uint8_t> CanonicalBytes;
	};

	bool DecodeWireTransaction(
		const FDSPGCSFrozenAssignmentWireTransaction& Transaction,
		FDecodedWireTransaction& OutDecoded,
		FString& OutError)
	{
		OutDecoded = {};
		OutError.Reset();
		if (Transaction.Frames.size() < 3)
		{
			OutError = TEXT("Transaction has too few frames.");
			return false;
		}

		std::vector<std::vector<std::uint8_t>> ChunkBytes;
		std::string DecodeError;
		for (std::size_t FrameIndex = 0;
			FrameIndex < Transaction.Frames.size();
			++FrameIndex)
		{
			dspgcs::MessageEnvelope Envelope;
			std::vector<std::uint8_t> PayloadBytes;
			if (!dspgcs::DecodeEnvelope(
				Transaction.Frames[FrameIndex], Envelope, PayloadBytes))
			{
				OutError = TEXT("DSP envelope decode failed.");
				return false;
			}
			if (FrameIndex == 0)
			{
				if (Envelope.messageType != dspgcs::FrozenAssignmentMessageTypes::Begin ||
					!dspgcs::DecodeFrozenAssignmentBegin(
						PayloadBytes, OutDecoded.Begin, DecodeError))
				{
					OutError = TEXT("Begin decode failed.");
					return false;
				}
				ChunkBytes.resize(OutDecoded.Begin.chunkCount);
			}
			else if (FrameIndex + 1 == Transaction.Frames.size())
			{
				if (Envelope.messageType != dspgcs::FrozenAssignmentMessageTypes::Commit ||
					!dspgcs::DecodeFrozenAssignmentCommit(
						PayloadBytes, OutDecoded.Commit, DecodeError))
				{
					OutError = TEXT("Commit decode failed.");
					return false;
				}
			}
			else
			{
				dspgcs::FrozenAssignmentChunkDTO Chunk;
				if (Envelope.messageType != dspgcs::FrozenAssignmentMessageTypes::Chunk ||
					!dspgcs::DecodeFrozenAssignmentChunk(
						PayloadBytes, Chunk, DecodeError) ||
					Chunk.chunkIndex >= ChunkBytes.size())
				{
					OutError = TEXT("Chunk decode failed.");
					return false;
				}
				ChunkBytes[Chunk.chunkIndex] = MoveTemp(Chunk.chunkPayloadBytes);
			}
		}

		for (const std::vector<std::uint8_t>& Chunk : ChunkBytes)
		{
			if (Chunk.empty())
			{
				OutError = TEXT("Chunk reconstruction has a gap.");
				return false;
			}
			OutDecoded.CanonicalBytes.insert(
				OutDecoded.CanonicalBytes.end(), Chunk.begin(), Chunk.end());
		}
		if (!dspgcs::DecodeCanonicalFrozenAssignmentPayload(
			OutDecoded.CanonicalBytes, OutDecoded.Payload, DecodeError))
		{
			OutError = TEXT("Canonical payload decode failed.");
			return false;
		}
		return true;
	}

	dspgcs::FrozenAssignmentAckDTO MakeAck(
		const FDSPGCSFrozenAssignmentWireTransaction& Transaction,
		const dspgcs::FrozenAssignmentAckResult Result =
			dspgcs::FrozenAssignmentAckResult::Accepted,
		const dspgcs::FrozenAssignmentErrorCode ErrorCode =
			dspgcs::FrozenAssignmentErrorCode::None)
	{
		dspgcs::FrozenAssignmentAckDTO Ack;
		Ack.transactionId = TCHAR_TO_UTF8(*Transaction.TransactionID);
		Ack.identity = Transaction.Identity;
		Ack.result = Result;
		Ack.errorCode = ErrorCode;
		Ack.acceptedDigest = Transaction.Digest;
		Ack.acceptedMatchedCount = Transaction.MatchedCount;
		return Ack;
	}

	dspgcs::MessageEnvelope MakeAckEnvelope(
		const FDSPGCSFrozenAssignmentWireTransaction& Transaction)
	{
		dspgcs::MessageEnvelope Envelope;
		Envelope.messageType = dspgcs::FrozenAssignmentMessageTypes::Ack;
		Envelope.correlationId = TCHAR_TO_UTF8(*Transaction.TransactionID);
		Envelope.sessionId = Transaction.Identity.fleetSessionId;
		return Envelope;
	}

	enum class ERealUdpFault
	{
		None,
		DropFirstAcceptedAck,
		DropFirstBegin,
		DropFirstMiddleChunk,
		DropFirstCommit,
		DuplicateBegin,
		DuplicateChunk,
		DuplicateCommit,
		ReorderChunks,
		Combined,
		CorruptChunk,
		DelayOldAcceptedAck
	};

	enum class ERealUdpGateMode
	{
		Single,
		DelayedOldAck,
		Atomicity,
		ConsoleRestart
	};

	struct FRealUdpRelaySnapshot
	{
		int32 UEFrozenFrames = 0;
		int32 ConsoleAckFrames = 0;
		int32 AcceptedAcks = 0;
		int32 RejectedAcks = 0;
		int32 MaximumFrozenDatagramBytes = 0;
		int64 UEFrozenDatagramBytes = 0;
		int64 ForwardedFrozenDatagramBytes = 0;
		bool bCorrelationValid = true;
		bool bSequenceValid = true;
		bool bReordered = false;
		bool bDelayedOldAckReleased = false;
	};

	class FRealUdpFaultRelay final : public FRunnable
	{
	public:
		FRealUdpFaultRelay(
			const uint16 InUEPort,
			const uint16 InConsolePort,
			const uint16 InUERelayPort,
			const uint16 InConsoleRelayPort,
			const ERealUdpFault InFault)
			: UEPort(InUEPort)
			, ConsolePort(InConsolePort)
			, UERelayPort(InUERelayPort)
			, ConsoleRelayPort(InConsoleRelayPort)
			, Fault(InFault)
		{
		}

		~FRealUdpFaultRelay() override
		{
			Shutdown();
		}

		bool StartRelay()
		{
			ISocketSubsystem* SocketSubsystem =
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (!SocketSubsystem)
			{
				return false;
			}
			UESocket = CreateBoundSocket(*SocketSubsystem, UERelayPort, TEXT("DSPGCS-E2E-UE-Relay"));
			ConsoleSocket = CreateBoundSocket(
				*SocketSubsystem, ConsoleRelayPort, TEXT("DSPGCS-E2E-Console-Relay"));
			if (!UESocket || !ConsoleSocket)
			{
				Shutdown();
				return false;
			}
			UEDestination = MakeAddress(*SocketSubsystem, UEPort);
			ConsoleDestination = MakeAddress(*SocketSubsystem, ConsolePort);
			if (!UEDestination.IsValid() || !ConsoleDestination.IsValid())
			{
				Shutdown();
				return false;
			}
			bStopRequested.store(false);
			Thread = FRunnableThread::Create(this, TEXT("DSPGCSFrozenAssignmentE2ERelay"));
			return Thread != nullptr;
		}

		void Stop() override
		{
			bStopRequested.store(true);
		}

		void Shutdown()
		{
			Stop();
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
			ISocketSubsystem* SocketSubsystem =
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (SocketSubsystem)
			{
				if (UESocket)
				{
					UESocket->Close();
					SocketSubsystem->DestroySocket(UESocket);
					UESocket = nullptr;
				}
				if (ConsoleSocket)
				{
					ConsoleSocket->Close();
					SocketSubsystem->DestroySocket(ConsoleSocket);
					ConsoleSocket = nullptr;
				}
			}
		}

		uint32 Run() override
		{
			while (!bStopRequested.load())
			{
				bool bDidWork = PumpSocket(UESocket, true);
				bDidWork = PumpSocket(ConsoleSocket, false) || bDidWork;
				if (!bDidWork)
				{
					FPlatformProcess::SleepNoStats(0.001f);
				}
			}
			return 0;
		}

		bool HasForwardedAck() const
		{
			return ForwardedAckCount.load() > 0;
		}

		void ArmOneChunkCorruption()
		{
			bArmedChunkCorruption.store(true);
		}

		FRealUdpRelaySnapshot GetSnapshot() const
		{
			FRealUdpRelaySnapshot Result;
			Result.UEFrozenFrames = UEFrozenFrames.load();
			Result.ConsoleAckFrames = ConsoleAckFrames.load();
			Result.AcceptedAcks = AcceptedAcks.load();
			Result.RejectedAcks = RejectedAcks.load();
			Result.MaximumFrozenDatagramBytes = MaximumFrozenDatagramBytes.load();
			Result.UEFrozenDatagramBytes = UEFrozenDatagramBytes.load();
			Result.ForwardedFrozenDatagramBytes = ForwardedFrozenDatagramBytes.load();
			Result.bCorrelationValid = bCorrelationValid.load();
			Result.bSequenceValid = bSequenceValid.load();
			Result.bReordered = bReordered.load();
			Result.bDelayedOldAckReleased = bDelayedOldAckReleased.load();
			return Result;
		}

	private:
		static TSharedPtr<FInternetAddr> MakeAddress(
			ISocketSubsystem& SocketSubsystem,
			const uint16 Port)
		{
			TSharedRef<FInternetAddr> Address = SocketSubsystem.CreateInternetAddr();
			bool bValid = false;
			Address->SetIp(TEXT("127.0.0.1"), bValid);
			Address->SetPort(Port);
			return bValid ? TSharedPtr<FInternetAddr>(Address) : nullptr;
		}

		static FSocket* CreateBoundSocket(
			ISocketSubsystem& SocketSubsystem,
			const uint16 Port,
			const TCHAR* Description)
		{
			FSocket* Socket = SocketSubsystem.CreateSocket(NAME_DGram, Description, false);
			if (!Socket)
			{
				return nullptr;
			}
			const TSharedPtr<FInternetAddr> Address = MakeAddress(SocketSubsystem, Port);
			bool bReuseAddress = false;
			Socket->SetReuseAddr(bReuseAddress);
			Socket->SetNonBlocking(true);
			int32 BufferSize = 4 * 1024 * 1024;
			Socket->SetReceiveBufferSize(BufferSize, BufferSize);
			Socket->SetSendBufferSize(BufferSize, BufferSize);
			if (!Address.IsValid() || !Socket->Bind(*Address))
			{
				Socket->Close();
				SocketSubsystem.DestroySocket(Socket);
				return nullptr;
			}
			return Socket;
		}

		bool PumpSocket(FSocket* Socket, const bool bFromUE)
		{
			if (!Socket)
			{
				return false;
			}
			bool bDidWork = false;
			uint32 PendingBytes = 0;
			while (Socket->HasPendingData(PendingBytes))
			{
				bDidWork = true;
				TArray<uint8> Bytes;
				Bytes.SetNumUninitialized(static_cast<int32>(FMath::Min<uint32>(PendingBytes, 65507)));
				TSharedRef<FInternetAddr> Source =
					ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
				int32 BytesRead = 0;
				if (!Socket->RecvFrom(Bytes.GetData(), Bytes.Num(), BytesRead, *Source) || BytesRead <= 0)
				{
					continue;
				}
				Bytes.SetNum(BytesRead, EAllowShrinking::No);
				std::vector<std::uint8_t> Frame(Bytes.GetData(), Bytes.GetData() + Bytes.Num());
				if (bFromUE)
				{
					HandleFromUE(MoveTemp(Frame));
				}
				else
				{
					HandleFromConsole(MoveTemp(Frame));
				}
			}
			return bDidWork;
		}

		void ObserveFrozenDatagram(const std::size_t Size)
		{
			int32 Current = MaximumFrozenDatagramBytes.load();
			while (Current < static_cast<int32>(Size) &&
				!MaximumFrozenDatagramBytes.compare_exchange_weak(
					Current, static_cast<int32>(Size)))
			{
			}
		}

		bool ForwardToConsole(const std::vector<std::uint8_t>& Frame, const bool bFrozen)
		{
			int32 BytesSent = 0;
			const bool bSent = ConsoleSocket && ConsoleDestination.IsValid() &&
				ConsoleSocket->SendTo(
					Frame.data(), static_cast<int32>(Frame.size()), BytesSent, *ConsoleDestination) &&
				BytesSent == static_cast<int32>(Frame.size());
			if (bSent && bFrozen)
			{
				ForwardedFrozenDatagramBytes.fetch_add(BytesSent);
			}
			return bSent;
		}

		bool ForwardToUE(const std::vector<std::uint8_t>& Frame, const bool bAck)
		{
			int32 BytesSent = 0;
			const bool bSent = UESocket && UEDestination.IsValid() &&
				UESocket->SendTo(
					Frame.data(), static_cast<int32>(Frame.size()), BytesSent, *UEDestination) &&
				BytesSent == static_cast<int32>(Frame.size());
			if (bSent && bAck)
			{
				ForwardedFrozenDatagramBytes.fetch_add(BytesSent);
				ForwardedAckCount.fetch_add(1);
			}
			return bSent;
		}

		void HandleFromUE(std::vector<std::uint8_t> Frame)
		{
			dspgcs::MessageEnvelope Envelope;
			std::vector<std::uint8_t> Payload;
			if (!dspgcs::DecodeEnvelope(Frame, Envelope, Payload) ||
				!DSPGCSFrozenAssignmentMessageType(Envelope.messageType))
			{
				ForwardToConsole(Frame, false);
				return;
			}

			ObserveFrozenDatagram(Frame.size());
			UEFrozenFrames.fetch_add(1);
			UEFrozenDatagramBytes.fetch_add(static_cast<int64>(Frame.size()));
			if (Envelope.sequence == 0)
			{
				bSequenceValid.store(false);
			}

			std::string TransactionID;
			std::string Error;
			if (Envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Begin)
			{
				dspgcs::FrozenAssignmentBeginDTO Begin;
				if (!dspgcs::DecodeFrozenAssignmentBegin(Payload, Begin, Error))
				{
					bCorrelationValid.store(false);
					return;
				}
				TransactionID = Begin.transactionId;
				if (!CurrentTransactionID.empty() && TransactionID != CurrentTransactionID &&
					!DelayedAcceptedAck.empty())
				{
					ForwardToUE(DelayedAcceptedAck, true);
					DelayedAcceptedAck.clear();
					bDelayedOldAckReleased.store(true);
				}
				CurrentTransactionID = TransactionID;
				if (Fault == ERealUdpFault::DropFirstBegin && !bPrimaryFaultApplied)
				{
					bPrimaryFaultApplied = true;
					return;
				}
				if (Fault == ERealUdpFault::DuplicateBegin && !bPrimaryFaultApplied)
				{
					bPrimaryFaultApplied = true;
					ForwardToConsole(Frame, true);
				}
			}
			else if (Envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Chunk)
			{
				dspgcs::FrozenAssignmentChunkDTO Chunk;
				if (!dspgcs::DecodeFrozenAssignmentChunk(Payload, Chunk, Error))
				{
					bCorrelationValid.store(false);
					return;
				}
				TransactionID = Chunk.transactionId;
				if (Fault == ERealUdpFault::DropFirstMiddleChunk && !bPrimaryFaultApplied &&
					Chunk.chunkCount > 1 && Chunk.chunkIndex == Chunk.chunkCount / 2)
				{
					bPrimaryFaultApplied = true;
					return;
				}
				if (Fault == ERealUdpFault::DuplicateChunk && !bPrimaryFaultApplied)
				{
					bPrimaryFaultApplied = true;
					ForwardToConsole(Frame, true);
				}
				const bool bArmedCorruption =
					!Chunk.chunkPayloadBytes.empty() && bArmedChunkCorruption.exchange(false);
				if (((Fault == ERealUdpFault::CorruptChunk) && !bPrimaryFaultApplied &&
					!Chunk.chunkPayloadBytes.empty()) || bArmedCorruption)
				{
					if (Fault == ERealUdpFault::CorruptChunk)
					{
						bPrimaryFaultApplied = true;
					}
					Chunk.chunkPayloadBytes[0] ^= 0x01;
					std::vector<std::uint8_t> CorruptPayload;
					if (dspgcs::EncodeFrozenAssignmentChunk(Chunk, CorruptPayload, Error))
					{
						Frame = dspgcs::MakeFrame(Envelope, CorruptPayload);
					}
				}
				if ((Fault == ERealUdpFault::ReorderChunks ||
					Fault == ERealUdpFault::Combined) && !bReorderAttemptComplete)
				{
					BufferedChunks.push_back(MoveTemp(Frame));
					return;
				}
			}
			else if (Envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Commit)
			{
				dspgcs::FrozenAssignmentCommitDTO Commit;
				if (!dspgcs::DecodeFrozenAssignmentCommit(Payload, Commit, Error))
				{
					bCorrelationValid.store(false);
					return;
				}
				TransactionID = Commit.transactionId;
				if (Fault == ERealUdpFault::DropFirstCommit && !bPrimaryFaultApplied)
				{
					bPrimaryFaultApplied = true;
					return;
				}
				if ((Fault == ERealUdpFault::ReorderChunks ||
					Fault == ERealUdpFault::Combined) && !bReorderAttemptComplete)
				{
					std::reverse(BufferedChunks.begin(), BufferedChunks.end());
					for (std::size_t Index = 0; Index < BufferedChunks.size(); ++Index)
					{
						ForwardToConsole(BufferedChunks[Index], true);
						if (Fault == ERealUdpFault::Combined && Index == 0)
						{
							ForwardToConsole(BufferedChunks[Index], true);
						}
					}
					BufferedChunks.clear();
					bReorderAttemptComplete = true;
					bReordered.store(true);
				}
				if (Fault == ERealUdpFault::DuplicateCommit && !bPrimaryFaultApplied)
				{
					bPrimaryFaultApplied = true;
					ForwardToConsole(Frame, true);
				}
			}

			if (Envelope.correlationId.empty() || Envelope.correlationId != TransactionID)
			{
				bCorrelationValid.store(false);
			}
			ForwardToConsole(Frame, true);
		}

		void HandleFromConsole(std::vector<std::uint8_t> Frame)
		{
			dspgcs::MessageEnvelope Envelope;
			std::vector<std::uint8_t> Payload;
			if (!dspgcs::DecodeEnvelope(Frame, Envelope, Payload) ||
				Envelope.messageType != dspgcs::FrozenAssignmentMessageTypes::Ack)
			{
				ForwardToUE(Frame, false);
				return;
			}
			ObserveFrozenDatagram(Frame.size());
			ConsoleAckFrames.fetch_add(1);
			dspgcs::FrozenAssignmentAckDTO Ack;
			std::string Error;
			if (!dspgcs::DecodeFrozenAssignmentAck(Payload, Ack, Error) ||
				Envelope.correlationId != Ack.transactionId)
			{
				bCorrelationValid.store(false);
				return;
			}
			if (Ack.result == dspgcs::FrozenAssignmentAckResult::Accepted)
			{
				AcceptedAcks.fetch_add(1);
				if ((Fault == ERealUdpFault::DropFirstAcceptedAck ||
					Fault == ERealUdpFault::Combined) && !bAckFaultApplied)
				{
					bAckFaultApplied = true;
					return;
				}
				if (Fault == ERealUdpFault::DelayOldAcceptedAck && DelayedAcceptedAck.empty())
				{
					DelayedAcceptedAck = Frame;
				}
			}
			else
			{
				RejectedAcks.fetch_add(1);
			}
			ForwardToUE(Frame, true);
		}

		static bool DSPGCSFrozenAssignmentMessageType(const dspgcs::MessageType Type)
		{
			return Type == dspgcs::FrozenAssignmentMessageTypes::Begin ||
				Type == dspgcs::FrozenAssignmentMessageTypes::Chunk ||
				Type == dspgcs::FrozenAssignmentMessageTypes::Commit ||
				Type == dspgcs::FrozenAssignmentMessageTypes::Abort;
		}

		uint16 UEPort = 0;
		uint16 ConsolePort = 0;
		uint16 UERelayPort = 0;
		uint16 ConsoleRelayPort = 0;
		ERealUdpFault Fault = ERealUdpFault::None;
		FSocket* UESocket = nullptr;
		FSocket* ConsoleSocket = nullptr;
		TSharedPtr<FInternetAddr> UEDestination;
		TSharedPtr<FInternetAddr> ConsoleDestination;
		FRunnableThread* Thread = nullptr;
		std::atomic<bool> bStopRequested{false};
		std::atomic<int32> UEFrozenFrames{0};
		std::atomic<int32> ConsoleAckFrames{0};
		std::atomic<int32> AcceptedAcks{0};
		std::atomic<int32> RejectedAcks{0};
		std::atomic<int32> ForwardedAckCount{0};
		std::atomic<int32> MaximumFrozenDatagramBytes{0};
		std::atomic<int64> UEFrozenDatagramBytes{0};
		std::atomic<int64> ForwardedFrozenDatagramBytes{0};
		std::atomic<bool> bCorrelationValid{true};
		std::atomic<bool> bSequenceValid{true};
		std::atomic<bool> bReordered{false};
		std::atomic<bool> bDelayedOldAckReleased{false};
		std::atomic<bool> bArmedChunkCorruption{false};
		bool bPrimaryFaultApplied = false;
		bool bAckFaultApplied = false;
		bool bReorderAttemptComplete = false;
		std::string CurrentTransactionID;
		std::vector<std::vector<std::uint8_t>> BufferedChunks;
		std::vector<std::uint8_t> DelayedAcceptedAck;
	};

	uint16 AllocateE2EPort()
	{
		ISocketSubsystem* SocketSubsystem =
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return 0;
		}
		FSocket* Socket = SocketSubsystem->CreateSocket(
			NAME_DGram, TEXT("DSPGCS-E2E-Port-Probe"), false);
		if (!Socket)
		{
			return 0;
		}
		TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
		bool bValid = false;
		Address->SetIp(TEXT("127.0.0.1"), bValid);
		Address->SetPort(0);
		uint16 Port = 0;
		if (bValid && Socket->Bind(*Address))
		{
			TSharedRef<FInternetAddr> BoundAddress = SocketSubsystem->CreateInternetAddr();
			Socket->GetAddress(*BoundAddress);
			Port = static_cast<uint16>(BoundAddress->GetPort());
		}
		Socket->Close();
		SocketSubsystem->DestroySocket(Socket);
		return Port;
	}

	TMap<FString, FString> ReadE2EResult(const FString& Path)
	{
		FString Contents;
		TMap<FString, FString> Result;
		if (!FFileHelper::LoadFileToString(Contents, *Path))
		{
			return Result;
		}
		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, true);
		for (const FString& Line : Lines)
		{
			FString Key;
			FString Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Result.Add(MoveTemp(Key), MoveTemp(Value));
			}
		}
		return Result;
	}

	int64 ResultInt(const TMap<FString, FString>& Result, const TCHAR* Key)
	{
		const FString* Value = Result.Find(Key);
		return Value ? FCString::Atoi64(**Value) : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSFrozenAssignmentSyncTests,
	"DroneGCS.RouteAssignment.FrozenSync.WireRetryStateValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSFrozenAssignmentSyncTests::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using EState = EDSPGCSFrozenAssignmentSyncState;
	using EError = dspgcs::FrozenAssignmentErrorCode;

	auto NewSync = []()
	{
		return NewObject<UDSPGCSFrozenAssignmentSyncSubsystem>(
			NewObject<UGameInstance>(GEngine));
	};
	auto Build = [this](
		const FRouteAssignmentSnapshot& Snapshot,
		const FString& TransactionID,
		FDSPGCSFrozenAssignmentWireTransaction& OutTransaction,
		FString& OutError,
		double* OutElapsedMs = nullptr)
	{
		std::uint64_t Sequence = 0;
		const double Started = FPlatformTime::Seconds();
		const bool bBuilt =
			UDSPGCSFrozenAssignmentSyncSubsystem::BuildWireTransaction(
				Snapshot, TransactionID, Sequence, OutTransaction, OutError);
		if (OutElapsedMs)
		{
			*OutElapsedMs = (FPlatformTime::Seconds() - Started) * 1000.0;
		}
		return bBuilt;
	};
	auto StartWithFake = [this](
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync,
		const FRouteAssignmentSnapshot& Snapshot,
		std::vector<std::vector<std::uint8_t>>& History,
		FString& OutError)
	{
		Sync->TestFrameSender = [&History](const std::vector<std::uint8_t>& Frame)
		{
			History.push_back(Frame);
			return true;
		};
		return Sync->StartFromSnapshot(
			Snapshot, Snapshot.SourceIdentity.FleetSessionID, false, OutError);
	};
	auto Accept = [this](UDSPGCSFrozenAssignmentSyncSubsystem* Sync)
	{
		const auto Ack = MakeAck(Sync->ActiveTransaction);
		const auto Envelope = MakeAckEnvelope(Sync->ActiveTransaction);
		Sync->HandleDecodedAck(Envelope, Ack, FPlatformTime::Seconds());
	};

	// Wire projection: Full, Degraded, partitions, canonical determinism and schema minimization.
	const FRouteAssignmentSnapshot Full = MakeFrozenSnapshot(4, false, 2, 1);
	FDSPGCSFrozenAssignmentWireTransaction FullTransaction;
	FString Error;
	TestTrue(TEXT("Full Frozen Snapshot builds"),
		Build(Full, TEXT("wire-full"), FullTransaction, Error));
	FDecodedWireTransaction FullDecoded;
	TestTrue(TEXT("Full transaction decodes"),
		DecodeWireTransaction(FullTransaction, FullDecoded, Error));
	TestEqual(TEXT("Begin is followed by chunks and Commit"),
		static_cast<int32>(FullTransaction.Frames.size()),
		static_cast<int32>(FullTransaction.ChunkCount + 2));
	TestEqual(TEXT("SpareCount projects"),
		static_cast<int32>(FullDecoded.Begin.spareCount), 2);
	TestEqual(TEXT("ExcludedCount projects"),
		static_cast<int32>(FullDecoded.Begin.excludedCount), 1);
	TestEqual(TEXT("Full VacantCount is zero"),
		static_cast<int32>(FullDecoded.Begin.vacantCount), 0);
	TestEqual(TEXT("Canonical mapping count"),
		static_cast<int32>(FullDecoded.Payload.mappings.size()), 4);
	TestTrue(TEXT("SourceDatasetDigest projects to Begin"),
		FullDecoded.Begin.sourceDatasetDigest ==
			FullDecoded.Payload.sourceDatasetDigest);
	TestEqual(TEXT("Normalized latitude projects to Begin"),
		FullDecoded.Begin.normalizedShowOriginLatitudeE7,
		FullDecoded.Payload.normalizedShowOriginLatitudeE7);
	TestEqual(TEXT("Normalized longitude projects to Begin"),
		FullDecoded.Begin.normalizedShowOriginLongitudeE7,
		FullDecoded.Payload.normalizedShowOriginLongitudeE7);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		TestEqual(
			*FString::Printf(TEXT("Canonical RouteSlot order %d"), Index),
			FullDecoded.Payload.mappings[Index].routeSlotIndex,
			Index);
	}

	FRouteAssignmentSnapshot NonWireFieldsChanged = Full;
	NonWireFieldsChanged.BindingProvenance =
		ERouteAssignmentBindingProvenance::Explicit;
	NonWireFieldsChanged.SourceIdentity.Latitude = -12.5;
	NonWireFieldsChanged.SourceIdentity.Longitude = 80.25;
	NonWireFieldsChanged.SourceIdentity.Altitude = 900.0;
	NonWireFieldsChanged.SourceIdentity.CapturedAtUtc = FDateTime(2030, 1, 1);
	for (FRouteAssignmentEntry& Entry : NonWireFieldsChanged.MatchedEntries)
	{
		Entry.HorizontalDistanceMeters = 0.0;
		Entry.bHorizontalDistanceValid = false;
	}
	FDSPGCSFrozenAssignmentWireTransaction NonWireChangedTransaction;
	TestTrue(TEXT("Non-wire source fields do not block projection"),
		Build(NonWireFieldsChanged, TEXT("wire-non-fields"),
			NonWireChangedTransaction, Error));
	FDecodedWireTransaction NonWireChangedDecoded;
	TestTrue(TEXT("Non-wire changed transaction decodes"),
		DecodeWireTransaction(
			NonWireChangedTransaction, NonWireChangedDecoded, Error));
	TestTrue(TEXT("Binding provenance, HorizontalDistance, raw ShowOrigin doubles and Geo are absent from canonical wire"),
		FullDecoded.CanonicalBytes == NonWireChangedDecoded.CanonicalBytes);
	TestTrue(TEXT("Non-wire fields do not alter digest"),
		FullTransaction.Digest == NonWireChangedTransaction.Digest);

	FDSPGCSFrozenAssignmentWireTransaction FullRepeat;
	TestTrue(TEXT("Same Full Snapshot rebuilds"),
		Build(Full, TEXT("wire-repeat"), FullRepeat, Error));
	FDecodedWireTransaction FullRepeatDecoded;
	TestTrue(TEXT("Repeated Full transaction decodes"),
		DecodeWireTransaction(FullRepeat, FullRepeatDecoded, Error));
	TestTrue(TEXT("Canonical bytes are deterministic"),
		FullDecoded.CanonicalBytes == FullRepeatDecoded.CanonicalBytes);
	TestTrue(TEXT("Digest is deterministic"),
		FullTransaction.Digest == FullRepeat.Digest);

	FRouteAssignmentSnapshot DifferentDataset = Full;
	uint8 DifferentDigestBytes[FRouteArtifactDigest256::ByteCount];
	FMemory::Memset(DifferentDigestBytes, 0x6b, sizeof(DifferentDigestBytes));
	FString DifferentDigestError;
	TestTrue(TEXT("Different test source digest is constructed"),
		FRouteArtifactDigest256::FromBytes(
			DifferentDigestBytes,
			UE_ARRAY_COUNT(DifferentDigestBytes),
			DifferentDataset.SourceIdentity.SourceDatasetDigest,
			DifferentDigestError));
	FDSPGCSFrozenAssignmentWireTransaction DifferentDatasetTransaction;
	TestTrue(TEXT("Different source dataset builds"),
		Build(DifferentDataset, TEXT("wire-different-dataset"),
			DifferentDatasetTransaction, Error));
	TestTrue(TEXT("Different SourceDatasetDigest changes AssignmentDigest"),
		DifferentDatasetTransaction.Digest != FullTransaction.Digest);

	FRouteAssignmentSnapshot DifferentOrigin = Full;
	++DifferentOrigin.SourceIdentity.NormalizedShowOriginLatitudeE7;
	FDSPGCSFrozenAssignmentWireTransaction DifferentOriginTransaction;
	TestTrue(TEXT("Different normalized origin builds"),
		Build(DifferentOrigin, TEXT("wire-different-origin"),
			DifferentOriginTransaction, Error));
	TestTrue(TEXT("Different normalized origin changes AssignmentDigest"),
		DifferentOriginTransaction.Digest != FullTransaction.Digest);

	FRouteAssignmentSnapshot DifferentGeneration = Full;
	++DifferentGeneration.SourceIdentity.RouteDatasetGeneration;
	FDSPGCSFrozenAssignmentWireTransaction DifferentGenerationTransaction;
	TestTrue(TEXT("Different runtime generation builds"),
		Build(DifferentGeneration, TEXT("wire-different-generation"),
			DifferentGenerationTransaction, Error));
	TestTrue(TEXT("Runtime generation preserves stable source tuple"),
		DifferentGeneration.SourceIdentity.SourceDatasetDigest ==
			Full.SourceIdentity.SourceDatasetDigest &&
		DifferentGeneration.SourceIdentity.NormalizedShowOriginLatitudeE7 ==
			Full.SourceIdentity.NormalizedShowOriginLatitudeE7 &&
		DifferentGeneration.SourceIdentity.NormalizedShowOriginLongitudeE7 ==
			Full.SourceIdentity.NormalizedShowOriginLongitudeE7 &&
		DifferentGeneration.RouteSlotCount == Full.RouteSlotCount);
	TestTrue(TEXT("Runtime generation remains in overall AssignmentDigest"),
		DifferentGenerationTransaction.Digest != FullTransaction.Digest);

	for (int32 InvalidKind = 0; InvalidKind < 6; ++InvalidKind)
	{
		FRouteAssignmentSnapshot InvalidStableSource = Full;
		switch (InvalidKind)
		{
		case 0:
			InvalidStableSource.SourceIdentity.SourceDatasetDigest.Reset();
			break;
		case 1:
		{
			uint8 ZeroDigestBytes[FRouteArtifactDigest256::ByteCount]{};
			FString ZeroDigestError;
			check(FRouteArtifactDigest256::FromBytes(
				ZeroDigestBytes,
				UE_ARRAY_COUNT(ZeroDigestBytes),
				InvalidStableSource.SourceIdentity.SourceDatasetDigest,
				ZeroDigestError));
			break;
		}
		case 2:
			InvalidStableSource.SourceIdentity.NormalizedShowOriginLatitudeE7 = -900000001;
			break;
		case 3:
			InvalidStableSource.SourceIdentity.NormalizedShowOriginLatitudeE7 = 900000001;
			break;
		case 4:
			InvalidStableSource.SourceIdentity.NormalizedShowOriginLongitudeE7 = -1800000001;
			break;
		default:
			InvalidStableSource.SourceIdentity.NormalizedShowOriginLongitudeE7 = 1800000001;
			break;
		}
		FDSPGCSFrozenAssignmentWireTransaction IgnoredTransaction;
		TestFalse(TEXT("Invalid stable source is rejected before wire encoding"),
			Build(InvalidStableSource, TEXT("wire-invalid-stable-source"),
				IgnoredTransaction, Error));
	}

	FRouteAssignmentSnapshot Reordered = Full;
	Algo::Reverse(Reordered.MatchedEntries);
	FDSPGCSFrozenAssignmentWireTransaction ReorderedTransaction;
	TestTrue(TEXT("Reordered semantic snapshot builds"),
		Build(Reordered, TEXT("wire-reordered"), ReorderedTransaction, Error));
	FDecodedWireTransaction ReorderedDecoded;
	TestTrue(TEXT("Reordered transaction decodes"),
		DecodeWireTransaction(ReorderedTransaction, ReorderedDecoded, Error));
	TestTrue(TEXT("Input mapping order does not alter canonical bytes"),
		FullDecoded.CanonicalBytes == ReorderedDecoded.CanonicalBytes);

	const FRouteAssignmentSnapshot Degraded = MakeFrozenSnapshot(3, true, 1, 2);
	FDSPGCSFrozenAssignmentWireTransaction DegradedTransaction;
	TestTrue(TEXT("Degraded Frozen Snapshot builds"),
		Build(Degraded, TEXT("wire-degraded"), DegradedTransaction, Error));
	FDecodedWireTransaction DegradedDecoded;
	TestTrue(TEXT("Degraded transaction decodes"),
		DecodeWireTransaction(DegradedTransaction, DegradedDecoded, Error));
	TestEqual(TEXT("Degraded VacantCount projects"),
		static_cast<int32>(DegradedDecoded.Begin.vacantCount), 1);
	TestTrue(TEXT("Degraded completion projects"),
		DegradedDecoded.Begin.completionMode ==
			dspgcs::FrozenAssignmentCompletionMode::Degraded);

	for (const std::vector<std::uint8_t>& Frame : FullTransaction.Frames)
	{
		TestTrue(TEXT("Small encoded frame is within datagram budget"),
			Frame.size() <= dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes);
	}
	TestEqual(TEXT("Reconstructed canonical byte count matches Begin"),
		static_cast<int64>(FullDecoded.CanonicalBytes.size()),
		static_cast<int64>(FullDecoded.Begin.totalPayloadBytes));

	// Public no-Frozen/FrozenLocal/Start/ManualRetry lifecycle path.
	UGameInstance* PublicGameInstance = NewObject<UGameInstance>(GEngine);
	PublicGameInstance->InitializeStandalone(TEXT("FrozenAssignmentSyncR2"));
	URouteAssignmentSubsystem* Authority =
		PublicGameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	UDSPGCSFrozenAssignmentSyncSubsystem* PublicSync =
		PublicGameInstance->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
	TestNotNull(TEXT("Public RouteAssignment authority exists"), Authority);
	TestNotNull(TEXT("Public Sync subsystem exists"), PublicSync);
	FString PublicError;
	TestFalse(TEXT("Start without Frozen fails"),
		PublicSync->StartSyncFrozenAssignment(PublicError));
	TestEqual(TEXT("No Frozen remains NotFrozen"),
		PublicSync->GetSyncState(), EState::NotFrozen);
	TestTrue(TEXT("Working assignment replacement succeeds"),
		Authority->ReplaceWorkingAssignment(ToCandidate(Full), PublicError));
	TestTrue(TEXT("Working assignment freezes"),
		Authority->Freeze(ERouteAssignmentFreezePolicy::RequireFull, PublicError));
	PublicSync->RefreshFrozenBinding();
	TestEqual(TEXT("Frozen without sync is FrozenLocal"),
		PublicSync->GetSyncState(), EState::FrozenLocal);
	std::vector<std::vector<std::uint8_t>> PublicHistory;
	PublicSync->TestFrameSender = [&PublicHistory](const std::vector<std::uint8_t>& Frame)
	{
		PublicHistory.push_back(Frame);
		return true;
	};
	TestTrue(TEXT("Public StartSync starts"),
		PublicSync->StartSyncFrozenAssignment(PublicError));
	TestEqual(TEXT("Public Start enters Syncing"),
		PublicSync->GetSyncState(), EState::SyncingToConsole);
	const FString FirstManualTransactionID = PublicSync->Status.TransactionID;
	const FString FirstManualDigest = PublicSync->Status.CanonicalPayloadDigest;
	const auto TerminalAck = MakeAck(
		PublicSync->ActiveTransaction,
		dspgcs::FrozenAssignmentAckResult::Rejected,
		EError::UnknownDroneID);
	PublicSync->HandleDecodedAck(
		MakeAckEnvelope(PublicSync->ActiveTransaction),
		TerminalAck,
		FPlatformTime::Seconds());
	TestEqual(TEXT("Terminal NAK enters SyncFailed"),
		PublicSync->GetSyncState(), EState::SyncFailed);
	TestTrue(TEXT("Manual RetryFailedSync starts"),
		PublicSync->RetryFailedSync(PublicError));
	TestNotEqual(TEXT("Manual retry generates a new TransactionID"),
		PublicSync->Status.TransactionID, FirstManualTransactionID);
	TestEqual(TEXT("Manual retry retains canonical digest"),
		PublicSync->Status.CanonicalPayloadDigest, FirstManualDigest);
	PublicSync->CancelActiveSync(PublicError);
	PublicGameInstance->Shutdown();

	// Valid ACK state and status binding.
	UDSPGCSFrozenAssignmentSyncSubsystem* AcceptedSync = NewSync();
	std::vector<std::vector<std::uint8_t>> AcceptedHistory;
	TestTrue(TEXT("Accepted scenario starts"),
		StartWithFake(AcceptedSync, Full, AcceptedHistory, Error));
	TestEqual(TEXT("Start enters SyncingToConsole"),
		AcceptedSync->Status.State, EState::SyncingToConsole);
	TestEqual(TEXT("Status binds FleetSessionID"),
		AcceptedSync->Status.FleetSessionID, Full.SourceIdentity.FleetSessionID);
	TestEqual(TEXT("Status binds AuthoritySessionID"),
		AcceptedSync->Status.AssignmentAuthoritySessionID,
		Full.AssignmentAuthoritySessionID);
	TestEqual(TEXT("Status binds AssignmentRevision"),
		AcceptedSync->Status.AssignmentRevision, Full.AssignmentRevision);
	TestFalse(TEXT("Status binds canonical digest"),
		AcceptedSync->Status.CanonicalPayloadDigest.IsEmpty());
	Accept(AcceptedSync);
	TestEqual(TEXT("Valid Accepted ACK enters ConsoleAccepted"),
		AcceptedSync->Status.State, EState::ConsoleAccepted);
	const FDSPGCSFrozenAssignmentSyncStatus AcceptedBaseline = AcceptedSync->Status;
	auto OldAfterAcceptedAck = MakeAck(
		AcceptedSync->ActiveTransaction,
		dspgcs::FrozenAssignmentAckResult::Rejected,
		EError::StaleRevision);
	OldAfterAcceptedAck.transactionId = "old-transaction";
	--OldAfterAcceptedAck.identity.assignmentRevision;
	auto OldAfterAcceptedEnvelope = MakeAckEnvelope(AcceptedSync->ActiveTransaction);
	OldAfterAcceptedEnvelope.correlationId = "old-transaction";
	AcceptedSync->HandleDecodedAck(
		OldAfterAcceptedEnvelope,
		OldAfterAcceptedAck,
		FPlatformTime::Seconds());
	TestEqual(TEXT("Old ACK cannot damage accepted state"),
		AcceptedSync->Status.State, EState::ConsoleAccepted);
	TestEqual(TEXT("Old ACK cannot replace accepted identity"),
		AcceptedSync->Status.AssignmentRevision,
		AcceptedBaseline.AssignmentRevision);
	FString CancelError;
	TestFalse(TEXT("Cancel after acceptance has no active transaction"),
		AcceptedSync->CancelActiveSync(CancelError));
	TestEqual(TEXT("Cancel cannot delete historical acceptance"),
		AcceptedSync->Status.State, EState::ConsoleAccepted);

	// ACK identity/correlation/digest/count validation.
	auto RunIgnoredAck = [this, &Full, &StartWithFake](
		const TCHAR* Label,
		const TFunction<void(
			dspgcs::MessageEnvelope&,
			dspgcs::FrozenAssignmentAckDTO&)>& Mutate)
	{
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync = NewObject<
			UDSPGCSFrozenAssignmentSyncSubsystem>(NewObject<UGameInstance>(GEngine));
		std::vector<std::vector<std::uint8_t>> History;
		FString LocalError;
		StartWithFake(Sync, Full, History, LocalError);
		auto Envelope = MakeAckEnvelope(Sync->ActiveTransaction);
		auto Ack = MakeAck(Sync->ActiveTransaction);
		Mutate(Envelope, Ack);
		Sync->HandleDecodedAck(Envelope, Ack, FPlatformTime::Seconds());
		TestEqual(Label, Sync->Status.State, EState::SyncingToConsole);
	};
	RunIgnoredAck(TEXT("Wrong Ack TransactionID is ignored"),
		[](auto&, auto& Ack) { Ack.transactionId = "wrong-transaction"; });
	RunIgnoredAck(TEXT("Wrong Envelope CorrelationID is ignored"),
		[](auto& Envelope, auto&) { Envelope.correlationId = "wrong-correlation"; });
	RunIgnoredAck(TEXT("Wrong FleetSessionID cannot accept"),
		[](auto&, auto& Ack) { Ack.identity.fleetSessionId = "wrong-fleet"; });
	RunIgnoredAck(TEXT("Wrong AuthoritySessionID cannot accept"),
		[](auto&, auto& Ack)
		{
			Ack.identity.assignmentAuthoritySessionId = "wrong-authority";
		});
	RunIgnoredAck(TEXT("Wrong AssignmentRevision cannot accept"),
		[](auto&, auto& Ack) { ++Ack.identity.assignmentRevision; });
	RunIgnoredAck(TEXT("Wrong AcceptedDigest cannot accept"),
		[](auto&, auto& Ack) { Ack.acceptedDigest[0] ^= 0xff; });
	RunIgnoredAck(TEXT("Wrong AcceptedMatchedCount cannot accept"),
		[](auto&, auto& Ack) { ++Ack.acceptedMatchedCount; });
	RunIgnoredAck(TEXT("Accepted with non-None error cannot accept"),
		[](auto&, auto& Ack) { Ack.errorCode = EError::HashMismatch; });

	// Retryable and terminal NAK classifications.
	for (const EError Retryable :
		{ EError::IncompleteTransaction, EError::HashMismatch })
	{
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync = NewSync();
		std::vector<std::vector<std::uint8_t>> History;
		TestTrue(TEXT("Retryable NAK scenario starts"),
			StartWithFake(Sync, Full, History, Error));
		const auto Ack = MakeAck(
			Sync->ActiveTransaction,
			dspgcs::FrozenAssignmentAckResult::Rejected,
			Retryable);
		Sync->HandleDecodedAck(
			MakeAckEnvelope(Sync->ActiveTransaction), Ack, 100.0);
		TestEqual(TEXT("Retryable NAK remains Syncing"),
			Sync->Status.State, EState::SyncingToConsole);
		TestTrue(TEXT("Retryable NAK schedules retry"), Sync->bRetryScheduled);
		const int32 AttemptOneFrames = static_cast<int32>(History.size());
		Sync->AdvanceRetryStateAt(Sync->RetryAtSeconds + 0.001);
		TestEqual(TEXT("Retryable NAK replays a complete transaction"),
			static_cast<int32>(History.size()), AttemptOneFrames * 2);
	}
	const EError TerminalErrors[] = {
		EError::UnsupportedProtocolVersion,
		EError::FleetSessionMismatch,
		EError::MalformedAssignment,
		EError::DuplicateDroneID,
		EError::DuplicateRouteSlot,
		EError::RouteSlotOutOfRange,
		EError::UnknownDroneID,
		EError::StaleRevision,
		EError::RevisionCollision,
		EError::InternalError,
		EError::StaleAuthoritySession
	};
	for (const EError Terminal : TerminalErrors)
	{
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync = NewSync();
		std::vector<std::vector<std::uint8_t>> History;
		StartWithFake(Sync, Full, History, Error);
		const int32 FramesBeforeNak = static_cast<int32>(History.size());
		const auto Ack = MakeAck(
			Sync->ActiveTransaction,
			dspgcs::FrozenAssignmentAckResult::Rejected,
			Terminal);
		Sync->HandleDecodedAck(
			MakeAckEnvelope(Sync->ActiveTransaction), Ack, 100.0);
		TestEqual(TEXT("Terminal NAK enters SyncFailed"),
			Sync->Status.State, EState::SyncFailed);
		Sync->AdvanceRetryStateAt(10000.0);
		TestEqual(TEXT("Terminal NAK does not send again"),
			static_cast<int32>(History.size()), FramesBeforeNak);
	}

	// ACK timeout replays the exact complete transaction with the same ID and bytes.
	UDSPGCSFrozenAssignmentSyncSubsystem* TimeoutSync = NewSync();
	std::vector<std::vector<std::uint8_t>> TimeoutHistory;
	StartWithFake(TimeoutSync, Full, TimeoutHistory, Error);
	const FString TimeoutTransactionID = TimeoutSync->Status.TransactionID;
	const auto TimeoutAttemptOne = TimeoutHistory;
	TimeoutSync->AdvanceRetryStateAt(TimeoutSync->AckDeadlineSeconds + 0.001);
	TimeoutSync->AdvanceRetryStateAt(TimeoutSync->RetryAtSeconds + 0.001);
	TestEqual(TEXT("Timeout retry increments AttemptCount"),
		TimeoutSync->Status.AttemptCount, 2);
	TestEqual(TEXT("Timeout retry retains TransactionID"),
		TimeoutSync->Status.TransactionID, TimeoutTransactionID);
	TestEqual(TEXT("Timeout retry replays full frame count"),
		static_cast<int32>(TimeoutHistory.size()),
		static_cast<int32>(TimeoutAttemptOne.size() * 2));
	for (std::size_t Index = 0; Index < TimeoutAttemptOne.size(); ++Index)
	{
		TestTrue(TEXT("Timeout retry reuses exact cached frame bytes"),
			TimeoutHistory[Index] == TimeoutHistory[Index + TimeoutAttemptOne.size()]);
	}
	Accept(TimeoutSync);
	TestEqual(TEXT("Idempotent ACK after lost ACK recovers acceptance"),
		TimeoutSync->Status.State, EState::ConsoleAccepted);

	// MaxAttempts exhaustion without sleeping.
	UDSPGCSFrozenAssignmentSyncSubsystem* ExhaustedSync = NewSync();
	std::vector<std::vector<std::uint8_t>> ExhaustedHistory;
	StartWithFake(ExhaustedSync, Full, ExhaustedHistory, Error);
	while (ExhaustedSync->Status.State == EState::SyncingToConsole)
	{
		ExhaustedSync->AdvanceRetryStateAt(
			ExhaustedSync->AckDeadlineSeconds + 0.001);
		if (ExhaustedSync->bRetryScheduled)
		{
			ExhaustedSync->AdvanceRetryStateAt(
				ExhaustedSync->RetryAtSeconds + 0.001);
		}
	}
	TestEqual(TEXT("MaxAttempts exhaustion enters SyncFailed"),
		ExhaustedSync->Status.State, EState::SyncFailed);
	TestEqual(TEXT("MaxAttempts is bounded at three"),
		ExhaustedSync->Status.AttemptCount, 3);
	const int32 ExhaustedFrames = static_cast<int32>(ExhaustedHistory.size());
	ExhaustedSync->AdvanceRetryStateAt(999999.0);
	TestEqual(TEXT("No send occurs after MaxAttempts failure"),
		static_cast<int32>(ExhaustedHistory.size()), ExhaustedFrames);

	// Immediate Begin/middle Chunk/Commit SendFrame failure each recovers by full replay.
	auto RunSendLoss = [this, &Full](const TCHAR* Label, const int32 FailIndexSelector)
	{
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync = NewObject<
			UDSPGCSFrozenAssignmentSyncSubsystem>(NewObject<UGameInstance>(GEngine));
		FDSPGCSFrozenAssignmentWireTransaction Probe;
		FString LocalError;
		std::uint64_t Sequence = 0;
		UDSPGCSFrozenAssignmentSyncSubsystem::BuildWireTransaction(
			Full, TEXT("loss-probe"), Sequence, Probe, LocalError);
		const int32 TransactionFrameCount = static_cast<int32>(Probe.Frames.size());
		const int32 FailIndex = FailIndexSelector < 0
			? TransactionFrameCount - 1
			: (FailIndexSelector == 1 ? TransactionFrameCount / 2 : 0);
		std::vector<std::vector<std::uint8_t>> History;
		int32 CallIndex = 0;
		bool bFailedOnce = false;
		Sync->TestFrameSender =
			[&](const std::vector<std::uint8_t>& Frame)
			{
				History.push_back(Frame);
				const bool bFail = !bFailedOnce && CallIndex == FailIndex;
				++CallIndex;
				if (bFail)
				{
					bFailedOnce = true;
					return false;
				}
				return true;
			};
		Sync->StartFromSnapshot(
			Full, Full.SourceIdentity.FleetSessionID, false, LocalError);
		TestTrue(Label, Sync->bRetryScheduled);
		Sync->AdvanceRetryStateAt(Sync->RetryAtSeconds + 0.001);
		TestEqual(TEXT("Recovered SendFrame failure reaches Attempt 2"),
			Sync->Status.AttemptCount, 2);
		TestTrue(TEXT("Recovered attempt sent a complete transaction"),
			History.size() >= static_cast<std::size_t>(TransactionFrameCount));
		const std::size_t ReplayOffset = History.size() - TransactionFrameCount;
		for (int32 FrameIndex = 0; FrameIndex < TransactionFrameCount; ++FrameIndex)
		{
			TestTrue(TEXT("Send failure retry replays cached transaction from Begin"),
				History[ReplayOffset + FrameIndex] == Sync->ActiveTransaction.Frames[FrameIndex]);
		}
	};
	RunSendLoss(TEXT("Begin SendFrame failure schedules full retry"), 0);
	RunSendLoss(TEXT("Middle Chunk SendFrame failure schedules full retry"), 1);
	RunSendLoss(TEXT("Commit SendFrame failure schedules full retry"), -1);

	UDSPGCSFrozenAssignmentSyncSubsystem* PersistentSendFailure = NewSync();
	int32 FailedSendCalls = 0;
	PersistentSendFailure->TestFrameSender =
		[&FailedSendCalls](const std::vector<std::uint8_t>&)
		{
			++FailedSendCalls;
			return false;
		};
	PersistentSendFailure->StartFromSnapshot(
		Full, Full.SourceIdentity.FleetSessionID, false, Error);
	while (PersistentSendFailure->Status.State == EState::SyncingToConsole)
	{
		PersistentSendFailure->AdvanceRetryStateAt(
			PersistentSendFailure->RetryAtSeconds + 0.001);
	}
	TestEqual(TEXT("Persistent SendFrame failure reaches SyncFailed"),
		PersistentSendFailure->Status.State, EState::SyncFailed);
	TestEqual(TEXT("Persistent SendFrame failure stops at MaxAttempts"),
		PersistentSendFailure->Status.AttemptCount, 3);
	TestEqual(TEXT("Each failed attempt restarts at Begin"),
		FailedSendCalls, 3);

	// Cancel emits best-effort Abort and exits Syncing locally.
	UDSPGCSFrozenAssignmentSyncSubsystem* CancelSync = NewSync();
	std::vector<std::vector<std::uint8_t>> CancelHistory;
	StartWithFake(CancelSync, Full, CancelHistory, Error);
	TestTrue(TEXT("Cancel active sync succeeds"),
		CancelSync->CancelActiveSync(Error));
	TestEqual(TEXT("Cancel exits Syncing"),
		CancelSync->Status.State, EState::FrozenLocal);
	dspgcs::MessageEnvelope AbortEnvelope;
	std::vector<std::uint8_t> AbortPayload;
	TestTrue(TEXT("Cancel sends a decodable Abort frame"),
		dspgcs::DecodeEnvelope(CancelHistory.back(), AbortEnvelope, AbortPayload));
	TestTrue(TEXT("Cancel frame is FrozenAssignmentAbort"),
		AbortEnvelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Abort);

	// ACK queue does not mutate state until GameThread processing.
	UDSPGCSFrozenAssignmentSyncSubsystem* QueueSync = NewSync();
	std::vector<std::vector<std::uint8_t>> QueueHistory;
	StartWithFake(QueueSync, Full, QueueHistory, Error);
	const auto QueueAck = MakeAck(QueueSync->ActiveTransaction);
	const auto QueueEnvelope = MakeAckEnvelope(QueueSync->ActiveTransaction);
	TFuture<void> EnqueueFuture = Async(EAsyncExecution::ThreadPool,
		[QueueSync, QueueEnvelope, QueueAck]()
		{
			QueueSync->EnqueueAck(QueueEnvelope, QueueAck);
		});
	EnqueueFuture.Wait();
	TestEqual(TEXT("Background ACK enqueue does not mutate state"),
		QueueSync->Status.State, EState::SyncingToConsole);
	QueueSync->ProcessPendingAcks();
	TestEqual(TEXT("GameThread ProcessPendingAcks applies acceptance"),
		QueueSync->Status.State, EState::ConsoleAccepted);

	// Accepted binding invalidation.
	auto MakeAcceptedSync = [this, &Full, &StartWithFake, &Accept]()
	{
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync = NewObject<
			UDSPGCSFrozenAssignmentSyncSubsystem>(NewObject<UGameInstance>(GEngine));
		std::vector<std::vector<std::uint8_t>> History;
		FString LocalError;
		StartWithFake(Sync, Full, History, LocalError);
		Accept(Sync);
		Sync->TestFrameSender = {};
		return Sync;
	};
	UDSPGCSFrozenAssignmentSyncSubsystem* SameBindingSync = MakeAcceptedSync();
	SameBindingSync->RefreshFromSnapshot(&Full, Full.SourceIdentity.FleetSessionID);
	TestEqual(TEXT("Identical Frozen binding remains accepted"),
		SameBindingSync->Status.State, EState::ConsoleAccepted);
	// Current FleetRevision is intentionally absent from this check: only the frozen
	// source audit revision participates in the digest.
	SameBindingSync->RefreshFromSnapshot(&Full, Full.SourceIdentity.FleetSessionID);
	TestEqual(TEXT("Current FleetRevision growth does not invalidate acceptance"),
		SameBindingSync->Status.State, EState::ConsoleAccepted);

	UDSPGCSFrozenAssignmentSyncSubsystem* FleetInvalidated = MakeAcceptedSync();
	FleetInvalidated->RefreshFromSnapshot(&Full, TEXT("FLEET-NEW"));
	TestEqual(TEXT("FleetSession change invalidates acceptance"),
		FleetInvalidated->Status.State, EState::FrozenLocal);

	FRouteAssignmentSnapshot AuthorityChanged = Full;
	AuthorityChanged.AssignmentAuthoritySessionID = TEXT("AUTHORITY-NEW");
	UDSPGCSFrozenAssignmentSyncSubsystem* AuthorityInvalidated = MakeAcceptedSync();
	AuthorityInvalidated->RefreshFromSnapshot(
		&AuthorityChanged, AuthorityChanged.SourceIdentity.FleetSessionID);
	TestEqual(TEXT("AuthoritySession change invalidates acceptance"),
		AuthorityInvalidated->Status.State, EState::FrozenLocal);

	FRouteAssignmentSnapshot RevisionChanged = Full;
	++RevisionChanged.AssignmentRevision;
	UDSPGCSFrozenAssignmentSyncSubsystem* RevisionInvalidated = MakeAcceptedSync();
	RevisionInvalidated->RefreshFromSnapshot(
		&RevisionChanged, RevisionChanged.SourceIdentity.FleetSessionID);
	TestEqual(TEXT("AssignmentRevision change invalidates acceptance"),
		RevisionInvalidated->Status.State, EState::FrozenLocal);

	FRouteAssignmentSnapshot DigestChanged = Full;
	DigestChanged.MatchedEntries[0].DroneID = TEXT("DroneDigestChanged");
	UDSPGCSFrozenAssignmentSyncSubsystem* DigestInvalidated = MakeAcceptedSync();
	DigestInvalidated->RefreshFromSnapshot(
		&DigestChanged, DigestChanged.SourceIdentity.FleetSessionID);
	TestEqual(TEXT("Canonical digest change invalidates acceptance"),
		DigestInvalidated->Status.State, EState::FrozenLocal);

	UDSPGCSFrozenAssignmentSyncSubsystem* RemovedFrozen = MakeAcceptedSync();
	RemovedFrozen->RefreshFromSnapshot(nullptr, FString());
	TestEqual(TEXT("Removed Frozen Snapshot becomes NotFrozen"),
		RemovedFrozen->Status.State, EState::NotFrozen);

	// 1K and 10K projection/chunk/send/acceptance scalability without per-item UObject.
	auto RunScale = [this, &Build, &StartWithFake, &Accept](const int32 MappingCount)
	{
		const FRouteAssignmentSnapshot ScaleSnapshot = MakeFrozenSnapshot(MappingCount);
		FDSPGCSFrozenAssignmentWireTransaction Transaction;
		FString LocalError;
		double BuildElapsedMs = 0.0;
		TestTrue(TEXT("Scale transaction builds"),
			Build(
				ScaleSnapshot,
				MappingCount == 1000
					? TEXT("00000000-0000-0000-0000-000000001000")
					: TEXT("00000000-0000-0000-0000-000000010000"),
				Transaction,
				LocalError,
				&BuildElapsedMs));
		FDecodedWireTransaction Decoded;
		TestTrue(TEXT("Scale transaction canonical payload reconstructs"),
			DecodeWireTransaction(Transaction, Decoded, LocalError));
		TestEqual(TEXT("Scale mapping count round-trips"),
			static_cast<int32>(Decoded.Payload.mappings.size()), MappingCount);
		for (const std::vector<std::uint8_t>& Frame : Transaction.Frames)
		{
			TestTrue(TEXT("Scale frame remains within datagram budget"),
				Frame.size() <= dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes);
		}

		std::size_t FakeBytes = 0;
		std::vector<std::vector<std::uint8_t>> FakeFrames;
		FakeFrames.reserve(Transaction.Frames.size());
		const double SendStarted = FPlatformTime::Seconds();
		for (const std::vector<std::uint8_t>& Frame : Transaction.Frames)
		{
			FakeBytes += Frame.size();
			FakeFrames.push_back(Frame);
		}
		const double FakeSendElapsedMs =
			(FPlatformTime::Seconds() - SendStarted) * 1000.0;

		UDSPGCSFrozenAssignmentSyncSubsystem* Sync = NewObject<
			UDSPGCSFrozenAssignmentSyncSubsystem>(NewObject<UGameInstance>(GEngine));
		std::vector<std::vector<std::uint8_t>> History;
		TestTrue(TEXT("Scale fake Sync starts"),
			StartWithFake(Sync, ScaleSnapshot, History, LocalError));
		Accept(Sync);
		TestEqual(TEXT("Scale Accepted ACK enters ConsoleAccepted"),
			Sync->Status.State, EState::ConsoleAccepted);

		AddInfo(FString::Printf(
			TEXT("R2_METRIC mappings=%d canonicalBytes=%lld chunks=%d frames=%d encodedBytes=%lld buildMs=%.3f fakeSendMs=%.3f"),
			MappingCount,
			static_cast<int64>(Transaction.CanonicalPayloadBytes),
			static_cast<int32>(Transaction.ChunkCount),
			static_cast<int32>(Transaction.Frames.size()),
			static_cast<int64>(FakeBytes),
			BuildElapsedMs,
			FakeSendElapsedMs));
	};
	RunScale(1000);
	RunScale(10000);

	struct FE2EScenarioResult
	{
		FString Name;
		EDSPGCSFrozenAssignmentSyncState State =
			EDSPGCSFrozenAssignmentSyncState::NotFrozen;
		int32 LastErrorCode = 0;
		int32 Attempts = 0;
		int32 MatchedCount = 0;
		int32 Chunks = 0;
		int32 Frames = 0;
		int64 CanonicalPayloadBytes = 0;
		int64 InitialEncodedBytes = 0;
		double ElapsedMilliseconds = 0.0;
		FString Digest;
		FString FleetSessionID;
		FString AuthoritySessionID;
		int64 AssignmentRevision = 0;
		FRealUdpRelaySnapshot Relay;
		TMap<FString, FString> Console;
		bool bQueueBoundaryVerified = false;
	};

	const FString ConsoleTestExecutable = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT(".."),
			TEXT("ConsoleGCS"),
			TEXT("x64"),
			TEXT("Release"),
			TEXT("ConsoleGCSTests.exe")));
	TestTrue(TEXT("Real UDP E2E Console server executable exists"),
		FPlatformFileManager::Get().GetPlatformFile().FileExists(*ConsoleTestExecutable));

	auto PumpGameThread = [](UDSPGCSFrozenAssignmentSyncSubsystem* Sync)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (Sync)
		{
			Sync->TickSync(0.0f);
		}
	};

	auto RunSingleRealUdpScenario = [
		this,
		&ConsoleTestExecutable,
		&PumpGameThread](
			const FString& Name,
			const int32 MappingCount,
			const bool bDegraded,
			const ERealUdpFault Fault,
			const bool bFleetSessionMismatch,
			const bool bVerifyQueueBoundary,
			const ERealUdpGateMode GateMode,
			FE2EScenarioResult& OutResult)
	{
		OutResult = {};
		OutResult.Name = Name;
		if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(
			*ConsoleTestExecutable))
		{
			AddError(TEXT("Console E2E server executable is missing."));
			return false;
		}

		TSet<uint16> Ports;
		while (Ports.Num() < 4)
		{
			const uint16 Port = AllocateE2EPort();
			if (Port != 0 && Port != dspgcs::DSPGCSDefaults::ConsoleBindPort &&
				Port != dspgcs::DSPGCSDefaults::UnrealBindPort)
			{
				Ports.Add(Port);
			}
		}
		TArray<uint16> PortArray = Ports.Array();
		const uint16 UEPort = PortArray[0];
		const uint16 ConsolePort = PortArray[1];
		const uint16 UERelayPort = PortArray[2];
		const uint16 ConsoleRelayPort = PortArray[3];

		FRealUdpFaultRelay Relay(
			UEPort, ConsolePort, UERelayPort, ConsoleRelayPort, Fault);
		if (!Relay.StartRelay())
		{
			AddError(FString::Printf(TEXT("%s: UDP relay failed to start."), *Name));
			return false;
		}

		const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString HarnessDirectory = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("FrozenE2E")));
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*HarnessDirectory);
		const FString ResultPath = FPaths::Combine(
			HarnessDirectory, Token + TEXT(".result"));
		const FString StopPath = FPaths::Combine(
			HarnessDirectory, Token + TEXT(".stop"));
		PlatformFile.DeleteFile(*ResultPath);
		PlatformFile.DeleteFile(*StopPath);

		UGameInstance* GameInstance = nullptr;
		FProcHandle ConsoleProcess;
		bool bStopRequested = false;
		ON_SCOPE_EXIT
		{
			if (!bStopRequested)
			{
				FFileHelper::SaveStringToFile(TEXT("stop"), *StopPath);
			}
			if (ConsoleProcess.IsValid())
			{
				const double WaitDeadline = FPlatformTime::Seconds() + 3.0;
				while (FPlatformProcess::IsProcRunning(ConsoleProcess) &&
					FPlatformTime::Seconds() < WaitDeadline)
				{
					FPlatformProcess::SleepNoStats(0.01f);
				}
				if (FPlatformProcess::IsProcRunning(ConsoleProcess))
				{
					FPlatformProcess::TerminateProc(ConsoleProcess, true);
				}
				FPlatformProcess::CloseProc(ConsoleProcess);
			}
			if (GameInstance)
			{
				GameInstance->Shutdown();
			}
			Relay.Shutdown();
			PlatformFile.DeleteFile(*ResultPath);
			PlatformFile.DeleteFile(*StopPath);
		};

		const TCHAR* Section = TEXT("DSPGCS.ProductionTelemetry");
		const TArray<FString> ConfigKeys = {
			TEXT("Enabled"), TEXT("TransportType"), TEXT("ConsoleAddress"),
			TEXT("UnrealAddress"), TEXT("ConsolePort"), TEXT("UnrealPort"),
			TEXT("ReceiveBufferBytes"), TEXT("ReceiverPollMs"),
			TEXT("TelemetryTimeoutMs") };
		TMap<FString, FString> PreviousConfig;
		TSet<FString> ExistingConfig;
		for (const FString& Key : ConfigKeys)
		{
			FString Value;
			if (GConfig->GetString(Section, *Key, Value, GGameIni))
			{
				ExistingConfig.Add(Key);
				PreviousConfig.Add(Key, MoveTemp(Value));
			}
		}
		GConfig->SetString(Section, TEXT("Enabled"), TEXT("True"), GGameIni);
		GConfig->SetString(Section, TEXT("TransportType"), TEXT("Loopback"), GGameIni);
		GConfig->SetString(Section, TEXT("ConsoleAddress"), TEXT("127.0.0.1"), GGameIni);
		GConfig->SetString(Section, TEXT("UnrealAddress"), TEXT("127.0.0.1"), GGameIni);
		GConfig->SetInt(Section, TEXT("ConsolePort"), UERelayPort, GGameIni);
		GConfig->SetInt(Section, TEXT("UnrealPort"), UEPort, GGameIni);
		GConfig->SetInt(Section, TEXT("ReceiveBufferBytes"), 4 * 1024 * 1024, GGameIni);
		GConfig->SetInt(Section, TEXT("ReceiverPollMs"), 1, GGameIni);
		GConfig->SetInt(Section, TEXT("TelemetryTimeoutMs"), 60000, GGameIni);

		GameInstance = NewObject<UGameInstance>(GEngine);
		GameInstance->InitializeStandalone(*FString::Printf(
			TEXT("FrozenAssignmentRealUdp-%s"), *Token));
		URouteAssignmentSubsystem* Authority =
			GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
		UFleetManagerSubsystem* Fleet =
			GameInstance->GetSubsystem<UFleetManagerSubsystem>();
		UDSPGCSProductionTelemetryProviderSubsystem* Provider =
			GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>();
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
			GameInstance->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();

		for (const FString& Key : ConfigKeys)
		{
			if (ExistingConfig.Contains(Key))
			{
				GConfig->SetString(Section, *Key, *PreviousConfig.FindChecked(Key), GGameIni);
			}
			else
			{
				GConfig->RemoveKey(Section, *Key, GGameIni);
			}
		}
		if (!Authority || !Fleet || !Provider || !Sync || !Provider->IsEnabled())
		{
			AddError(FString::Printf(TEXT("%s: production UE subsystems failed to initialize."), *Name));
			return false;
		}

		const FString Arguments = FString::Printf(
			TEXT("--frozen-e2e-server --console-port %d --remote-port %d --drones %d --result \"%s\" --stop \"%s\" --timeout 60"),
			ConsolePort,
			ConsoleRelayPort,
			MappingCount,
			*ResultPath,
			*StopPath);
		uint32 ConsoleProcessID = 0;
		ConsoleProcess = FPlatformProcess::CreateProc(
			*ConsoleTestExecutable,
			*Arguments,
			false,
			true,
			true,
			&ConsoleProcessID,
			0,
			*FPaths::GetPath(ConsoleTestExecutable),
			nullptr);
		if (!ConsoleProcess.IsValid())
		{
			AddError(FString::Printf(TEXT("%s: Console E2E process failed to launch."), *Name));
			return false;
		}

		const double FleetDeadline = FPlatformTime::Seconds() +
			(MappingCount >= 10000 ? 15.0 : 6.0);
		while ((Fleet->GetActiveFleetSessionID().IsEmpty() ||
			Provider->GetReceivedDroneCount() != MappingCount) &&
			FPlatformTime::Seconds() < FleetDeadline &&
			FPlatformProcess::IsProcRunning(ConsoleProcess))
		{
			PumpGameThread(Sync);
			FPlatformProcess::SleepNoStats(0.002f);
		}
		const FString CurrentFleetSessionID = Fleet->GetActiveFleetSessionID();
		if (CurrentFleetSessionID.IsEmpty() ||
			Provider->GetReceivedDroneCount() != MappingCount ||
			!Provider->IsTelemetryLinkConnected())
		{
			AddError(FString::Printf(
				TEXT("%s: production telemetry did not establish the test Fleet session (%d/%d)."),
				*Name, Provider->GetReceivedDroneCount(), MappingCount));
			return false;
		}

		FRouteAssignmentSnapshot Source = MakeFrozenSnapshot(MappingCount, bDegraded);
		Source.SourceIdentity.FleetSessionID = bFleetSessionMismatch
			? CurrentFleetSessionID + TEXT("-OLD")
			: CurrentFleetSessionID;
		Source.SourceIdentity.CapturedAtUtc = FDateTime::UtcNow();
		FString ScenarioError;
		if (GateMode == ERealUdpGateMode::Atomicity)
		{
			for (int32 Revision = 1; Revision < 5; ++Revision)
			{
				if (!Authority->ReplaceWorkingAssignment(ToCandidate(Source), ScenarioError) ||
					!Authority->Freeze(
						ERouteAssignmentFreezePolicy::RequireFull, ScenarioError))
				{
					AddError(FString::Printf(
						TEXT("%s: failed to prepare atomicity revision %d: %s"),
						*Name, Revision, *ScenarioError));
					return false;
				}
				Authority->Reset();
			}
		}
		if (!Authority->ReplaceWorkingAssignment(ToCandidate(Source), ScenarioError) ||
			!Authority->Freeze(
				bDegraded
					? ERouteAssignmentFreezePolicy::AllowDegraded
					: ERouteAssignmentFreezePolicy::RequireFull,
				ScenarioError))
		{
			AddError(FString::Printf(
				TEXT("%s: production RouteAssignment freeze failed: %s"),
				*Name, *ScenarioError));
			return false;
		}
		FRouteAssignmentSnapshot Frozen;
		if (!Authority->GetFrozenSnapshot(Frozen))
		{
			AddError(FString::Printf(TEXT("%s: Frozen Snapshot is unavailable."), *Name));
			return false;
		}

		const double Started = FPlatformTime::Seconds();
		const bool bStarted = bFleetSessionMismatch
			? Sync->StartFromSnapshot(Frozen, FString(), false, ScenarioError)
			: Sync->StartSyncFrozenAssignment(ScenarioError);
		if (!bStarted)
		{
			AddError(FString::Printf(
				TEXT("%s: production sync failed to start: %s"), *Name, *ScenarioError));
			return false;
		}
		OutResult.CanonicalPayloadBytes =
			static_cast<int64>(Sync->ActiveTransaction.CanonicalPayloadBytes);
		OutResult.InitialEncodedBytes =
			static_cast<int64>(Sync->ActiveTransaction.EncodedTotalBytes);
		OutResult.Frames = static_cast<int32>(Sync->ActiveTransaction.Frames.size());

		if (bVerifyQueueBoundary || bFleetSessionMismatch)
		{
			const double AckDeadline = FPlatformTime::Seconds() + 3.0;
			while (!Relay.HasForwardedAck() && FPlatformTime::Seconds() < AckDeadline)
			{
				FPlatformProcess::SleepNoStats(0.002f);
			}
			FPlatformProcess::SleepNoStats(0.02f);
			const bool bStillSyncingBeforeGameThreadDrain =
				Sync->Status.State == EState::SyncingToConsole;
			Sync->ProcessPendingAcks();
			OutResult.bQueueBoundaryVerified =
				Relay.HasForwardedAck() && bStillSyncingBeforeGameThreadDrain &&
				Sync->Status.State != EState::SyncingToConsole;
		}
		else
		{
			const double SyncDeadline = FPlatformTime::Seconds() +
				(MappingCount >= 10000 ? 15.0 : 6.0);
			while (Sync->Status.State == EState::SyncingToConsole &&
				FPlatformTime::Seconds() < SyncDeadline)
			{
				PumpGameThread(Sync);
				FPlatformProcess::SleepNoStats(0.002f);
			}
		}

		int64 AtomicBaselineRevision = 0;
		FString AtomicBaselineDigest;
		if (GateMode == ERealUdpGateMode::DelayedOldAck)
		{
			if (Sync->Status.State != EState::ConsoleAccepted)
			{
				AddError(TEXT("Delayed old ACK gate did not establish transaction A."));
				return false;
			}
			Authority->Reset();
			FRouteAssignmentSnapshot Second = MakeFrozenSnapshot(MappingCount, false);
			Second.SourceIdentity.FleetSessionID = CurrentFleetSessionID;
			Second.SourceIdentity.CapturedAtUtc = FDateTime::UtcNow();
			if (!Authority->ReplaceWorkingAssignment(ToCandidate(Second), ScenarioError) ||
				!Authority->Freeze(ERouteAssignmentFreezePolicy::RequireFull, ScenarioError) ||
				!Sync->StartSyncFrozenAssignment(ScenarioError))
			{
				AddError(FString::Printf(TEXT("Delayed old ACK transaction B failed: %s"), *ScenarioError));
				return false;
			}
			const double SecondDeadline = FPlatformTime::Seconds() + 5.0;
			while (Sync->Status.State == EState::SyncingToConsole &&
				FPlatformTime::Seconds() < SecondDeadline)
			{
				PumpGameThread(Sync);
				FPlatformProcess::SleepNoStats(0.002f);
			}
			if (Sync->Status.State != EState::ConsoleAccepted ||
				Sync->Status.AssignmentRevision != 2 ||
				!Relay.GetSnapshot().bDelayedOldAckReleased)
			{
				AddError(TEXT("Delayed old ACK changed or blocked transaction B."));
				return false;
			}
		}
		else if (GateMode == ERealUdpGateMode::Atomicity)
		{
			if (Sync->Status.State != EState::ConsoleAccepted ||
				Sync->Status.AssignmentRevision != 5)
			{
				AddError(TEXT("Atomicity gate did not establish accepted revision 5."));
				return false;
			}
			AtomicBaselineRevision = Sync->Status.AssignmentRevision;
			AtomicBaselineDigest = Sync->Status.CanonicalPayloadDigest;
			Authority->Reset();
			FRouteAssignmentSnapshot RevisionSix = MakeFrozenSnapshot(MappingCount, false);
			RevisionSix.SourceIdentity.FleetSessionID = CurrentFleetSessionID;
			RevisionSix.SourceIdentity.CapturedAtUtc = FDateTime::UtcNow();
			if (!Authority->ReplaceWorkingAssignment(ToCandidate(RevisionSix), ScenarioError) ||
				!Authority->Freeze(ERouteAssignmentFreezePolicy::RequireFull, ScenarioError))
			{
				AddError(FString::Printf(TEXT("Atomicity revision 6 freeze failed: %s"), *ScenarioError));
				return false;
			}
			Relay.ArmOneChunkCorruption();
			const int32 RejectedBefore = Relay.GetSnapshot().RejectedAcks;
			if (!Sync->StartSyncFrozenAssignment(ScenarioError))
			{
				AddError(FString::Printf(TEXT("Atomicity revision 6 sync failed: %s"), *ScenarioError));
				return false;
			}
			const double NakDeadline = FPlatformTime::Seconds() + 3.0;
			while (Relay.GetSnapshot().RejectedAcks <= RejectedBefore &&
				FPlatformTime::Seconds() < NakDeadline)
			{
				FPlatformProcess::SleepNoStats(0.002f);
			}
			Sync->ProcessPendingAcks();
			if (Sync->Status.State != EState::SyncingToConsole ||
				Sync->Status.AssignmentRevision != 6 ||
				Sync->Status.LastErrorCode != static_cast<int32>(EError::HashMismatch))
			{
				AddError(TEXT("Atomicity revision 6 did not remain retryable after HashMismatch."));
				return false;
			}
		}
		else if (GateMode == ERealUdpGateMode::ConsoleRestart)
		{
			if (Sync->Status.State != EState::ConsoleAccepted)
			{
				AddError(TEXT("Console restart gate did not establish session A acceptance."));
				return false;
			}
			const FString FleetSessionA = CurrentFleetSessionID;
			FFileHelper::SaveStringToFile(TEXT("stop"), *StopPath);
			const double FirstStopDeadline = FPlatformTime::Seconds() + 5.0;
			while (FPlatformProcess::IsProcRunning(ConsoleProcess) &&
				FPlatformTime::Seconds() < FirstStopDeadline)
			{
				FPlatformProcess::SleepNoStats(0.005f);
			}
			int32 FirstReturnCode = -1;
			if (!FPlatformProcess::GetProcReturnCode(ConsoleProcess, &FirstReturnCode) ||
				FirstReturnCode != 0 ||
				ResultInt(ReadE2EResult(ResultPath), TEXT("storePresent")) != 1)
			{
				AddError(TEXT("Console session A did not stop with an accepted store."));
				return false;
			}
			FPlatformProcess::CloseProc(ConsoleProcess);
			ConsoleProcess = FProcHandle();
			PlatformFile.DeleteFile(*StopPath);
			PlatformFile.DeleteFile(*ResultPath);
			FPlatformProcess::SleepNoStats(0.02f);
			ConsoleProcess = FPlatformProcess::CreateProc(
				*ConsoleTestExecutable,
				*Arguments,
				false,
				true,
				true,
				&ConsoleProcessID,
				0,
				*FPaths::GetPath(ConsoleTestExecutable),
				nullptr);
			if (!ConsoleProcess.IsValid())
			{
				AddError(TEXT("Console session B process failed to launch."));
				return false;
			}
			const double NewSessionDeadline = FPlatformTime::Seconds() + 6.0;
			while ((Fleet->GetActiveFleetSessionID().IsEmpty() ||
				Fleet->GetActiveFleetSessionID() == FleetSessionA) &&
				FPlatformTime::Seconds() < NewSessionDeadline)
			{
				PumpGameThread(Sync);
				FPlatformProcess::SleepNoStats(0.002f);
			}
			Sync->RefreshFrozenBinding();
			if (Fleet->GetActiveFleetSessionID().IsEmpty() ||
				Fleet->GetActiveFleetSessionID() == FleetSessionA ||
				Sync->Status.State != EState::FrozenLocal)
			{
				AddError(TEXT("Console session B did not invalidate the old accepted binding."));
				return false;
			}
			const int32 RejectedBefore = Relay.GetSnapshot().RejectedAcks;
			if (!Sync->StartFromSnapshot(Frozen, FString(), false, ScenarioError))
			{
				AddError(FString::Printf(TEXT("Old Frozen resend did not start: %s"), *ScenarioError));
				return false;
			}
			const double RestartNakDeadline = FPlatformTime::Seconds() + 3.0;
			while (Relay.GetSnapshot().RejectedAcks <= RejectedBefore &&
				FPlatformTime::Seconds() < RestartNakDeadline)
			{
				FPlatformProcess::SleepNoStats(0.002f);
			}
			Sync->ProcessPendingAcks();
			OutResult.bQueueBoundaryVerified =
				Sync->Status.State == EState::SyncFailed &&
				Sync->Status.LastErrorCode == static_cast<int32>(EError::FleetSessionMismatch);
			if (!OutResult.bQueueBoundaryVerified)
			{
				AddError(TEXT("Console session B did not terminally reject the old Frozen Assignment."));
				return false;
			}
		}

		OutResult.ElapsedMilliseconds =
			(FPlatformTime::Seconds() - Started) * 1000.0;
		OutResult.State = Sync->Status.State;
		OutResult.LastErrorCode = Sync->Status.LastErrorCode;
		OutResult.Attempts = Sync->Status.AttemptCount;
		OutResult.MatchedCount = Sync->Status.MatchedCount;
		OutResult.Chunks = Sync->Status.ChunkCount;
		OutResult.Digest = Sync->Status.CanonicalPayloadDigest;
		OutResult.FleetSessionID = Sync->Status.FleetSessionID;
		OutResult.AuthoritySessionID = Sync->Status.AssignmentAuthoritySessionID;
		OutResult.AssignmentRevision = Sync->Status.AssignmentRevision;
		OutResult.Relay = Relay.GetSnapshot();

		FFileHelper::SaveStringToFile(TEXT("stop"), *StopPath);
		bStopRequested = true;
		const double ProcessDeadline = FPlatformTime::Seconds() + 5.0;
		while (FPlatformProcess::IsProcRunning(ConsoleProcess) &&
			FPlatformTime::Seconds() < ProcessDeadline)
		{
			FPlatformProcess::SleepNoStats(0.005f);
		}
		int32 ProcessReturnCode = -1;
		if (!FPlatformProcess::GetProcReturnCode(ConsoleProcess, &ProcessReturnCode) ||
			ProcessReturnCode != 0)
		{
			AddError(FString::Printf(
				TEXT("%s: Console E2E server exit code=%d."), *Name, ProcessReturnCode));
			return false;
		}
		OutResult.Console = ReadE2EResult(ResultPath);
		if (OutResult.Console.IsEmpty())
		{
			AddError(FString::Printf(TEXT("%s: Console E2E result is missing."), *Name));
			return false;
		}

		const bool bExpectedAccepted = !bFleetSessionMismatch &&
			GateMode != ERealUdpGateMode::Atomicity &&
			GateMode != ERealUdpGateMode::ConsoleRestart;
		const bool bStateCorrect = bExpectedAccepted
			? OutResult.State == EState::ConsoleAccepted
			: (GateMode == ERealUdpGateMode::Atomicity
				? OutResult.State == EState::SyncingToConsole
				: OutResult.State == EState::SyncFailed);
		const bool bStoreCorrect = GateMode == ERealUdpGateMode::Atomicity
			? ResultInt(OutResult.Console, TEXT("storePresent")) == 1 &&
				ResultInt(OutResult.Console, TEXT("storeRevision")) == AtomicBaselineRevision &&
				OutResult.Console.FindRef(TEXT("storeDigest")) == AtomicBaselineDigest &&
				ResultInt(OutResult.Console, TEXT("storeMutationCount")) == 1
			: (bExpectedAccepted
			? ResultInt(OutResult.Console, TEXT("storePresent")) == 1 &&
				ResultInt(OutResult.Console, TEXT("storeMatched")) == MappingCount &&
				ResultInt(OutResult.Console, TEXT("mappingMismatches")) == 0 &&
				OutResult.Console.FindRef(TEXT("session")) == OutResult.FleetSessionID &&
				OutResult.Console.FindRef(TEXT("storeDigest")) == OutResult.Digest &&
				OutResult.Console.FindRef(TEXT("storeAuthority")) == OutResult.AuthoritySessionID &&
				ResultInt(OutResult.Console, TEXT("storeRevision")) == OutResult.AssignmentRevision &&
				(!bDegraded ||
					(ResultInt(OutResult.Console, TEXT("storeVacant")) == 1 &&
					 ResultInt(OutResult.Console, TEXT("storeCompletion")) ==
						 static_cast<int32>(dspgcs::FrozenAssignmentCompletionMode::Degraded)))
			: ResultInt(OutResult.Console, TEXT("storePresent")) == 0 &&
				ResultInt(OutResult.Console, TEXT("lastErrorCode")) ==
					static_cast<int32>(EError::FleetSessionMismatch));
		const bool bWireCorrect = OutResult.Relay.MaximumFrozenDatagramBytes <=
			static_cast<int32>(dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes) &&
			OutResult.Relay.bCorrelationValid && OutResult.Relay.bSequenceValid;
		bool bFaultCorrect = true;
		if (GateMode == ERealUdpGateMode::Single)
		{
			switch (Fault)
			{
			case ERealUdpFault::DropFirstAcceptedAck:
				bFaultCorrect = OutResult.Attempts == 2 &&
					ResultInt(OutResult.Console, TEXT("transactionsAccepted")) == 2 &&
					ResultInt(OutResult.Console, TEXT("storeMutationCount")) == 1;
				break;
			case ERealUdpFault::DropFirstBegin:
			case ERealUdpFault::DropFirstCommit:
				bFaultCorrect = OutResult.Attempts == 2 &&
					ResultInt(OutResult.Console, TEXT("storeMutationCount")) == 1;
				break;
			case ERealUdpFault::DropFirstMiddleChunk:
			case ERealUdpFault::CorruptChunk:
				bFaultCorrect = OutResult.Attempts == 2 &&
					ResultInt(OutResult.Console, TEXT("transactionsRejected")) >= 1 &&
					ResultInt(OutResult.Console, TEXT("storeMutationCount")) == 1;
				break;
			case ERealUdpFault::DuplicateBegin:
			case ERealUdpFault::DuplicateChunk:
			case ERealUdpFault::DuplicateCommit:
				bFaultCorrect = OutResult.Attempts == 1 &&
					ResultInt(OutResult.Console, TEXT("storeMutationCount")) == 1;
				break;
			case ERealUdpFault::ReorderChunks:
				bFaultCorrect = OutResult.Attempts == 1 && OutResult.Relay.bReordered;
				break;
			case ERealUdpFault::Combined:
				bFaultCorrect = OutResult.Attempts == 2 && OutResult.Relay.bReordered &&
					ResultInt(OutResult.Console, TEXT("storeMutationCount")) == 1;
				break;
			default:
				break;
			}
		}
		if (!bStateCorrect || !bStoreCorrect || !bWireCorrect || !bFaultCorrect ||
			((bVerifyQueueBoundary || bFleetSessionMismatch ||
				GateMode == ERealUdpGateMode::ConsoleRestart) &&
				!OutResult.bQueueBoundaryVerified))
		{
			AddError(FString::Printf(
				TEXT("%s: E2E validation failed state=%d attempts=%d store=%lld maxDatagram=%d queue=%d."),
				*Name,
				static_cast<int32>(OutResult.State),
				OutResult.Attempts,
				ResultInt(OutResult.Console, TEXT("storePresent")),
				OutResult.Relay.MaximumFrozenDatagramBytes,
				OutResult.bQueueBoundaryVerified ? 1 : 0));
			return false;
		}

		AddInfo(FString::Printf(
			TEXT("E2E_METRIC scenario=%s mappings=%d canonicalBytes=%lld chunks=%d frames=%d endpointUdpBytes=%lld relayForwardedFrozenBytes=%lld maxDatagram=%d attempts=%d elapsedMs=%.3f accepted=%lld rejected=%lld storeMutations=%lld"),
			*Name,
			MappingCount,
			OutResult.CanonicalPayloadBytes,
			OutResult.Chunks,
			OutResult.Frames,
			OutResult.Relay.UEFrozenDatagramBytes,
			OutResult.Relay.ForwardedFrozenDatagramBytes,
			OutResult.Relay.MaximumFrozenDatagramBytes,
			OutResult.Attempts,
			OutResult.ElapsedMilliseconds,
			ResultInt(OutResult.Console, TEXT("transactionsAccepted")),
			ResultInt(OutResult.Console, TEXT("transactionsRejected")),
			ResultInt(OutResult.Console, TEXT("storeMutationCount"))));
		return true;
	};

	TMap<FString, FE2EScenarioResult> E2EResults;
	auto RunE2E = [this, &RunSingleRealUdpScenario, &E2EResults](
		const TCHAR* Name,
		const int32 MappingCount,
		const bool bDegraded,
		const ERealUdpFault Fault,
		const bool bMismatch = false,
		const bool bQueueBoundary = false,
		const ERealUdpGateMode GateMode = ERealUdpGateMode::Single)
	{
		FE2EScenarioResult Result;
		const bool bPassed = RunSingleRealUdpScenario(
			Name, MappingCount, bDegraded, Fault, bMismatch, bQueueBoundary, GateMode, Result);
		TestTrue(*FString::Printf(TEXT("Real UDP E2E %s"), Name), bPassed);
		E2EResults.Add(Name, MoveTemp(Result));
	};

	RunE2E(TEXT("HappyFull"), 8, false, ERealUdpFault::None, false, true);
	RunE2E(TEXT("HappyDegraded"), 7, true, ERealUdpFault::None);
	RunE2E(TEXT("AckLoss"), 128, false, ERealUdpFault::DropFirstAcceptedAck);
	RunE2E(TEXT("BeginLoss"), 128, false, ERealUdpFault::DropFirstBegin);
	RunE2E(TEXT("MiddleChunkLoss"), 128, false, ERealUdpFault::DropFirstMiddleChunk);
	RunE2E(TEXT("CommitLoss"), 128, false, ERealUdpFault::DropFirstCommit);
	RunE2E(TEXT("DuplicateBegin"), 128, false, ERealUdpFault::DuplicateBegin);
	RunE2E(TEXT("DuplicateChunk"), 128, false, ERealUdpFault::DuplicateChunk);
	RunE2E(TEXT("DuplicateCommit"), 128, false, ERealUdpFault::DuplicateCommit);
	RunE2E(TEXT("ReorderedChunks"), 128, false, ERealUdpFault::ReorderChunks);
	RunE2E(TEXT("CombinedFault"), 128, false, ERealUdpFault::Combined);
	RunE2E(TEXT("HashCorruption"), 128, false, ERealUdpFault::CorruptChunk);
	RunE2E(TEXT("FleetSessionMismatch"), 8, false, ERealUdpFault::None, true, true);
	RunE2E(TEXT("OldDelayedAck"), 128, false,
		ERealUdpFault::DelayOldAcceptedAck, false, false,
		ERealUdpGateMode::DelayedOldAck);
	RunE2E(TEXT("Atomicity"), 128, false,
		ERealUdpFault::None, false, false, ERealUdpGateMode::Atomicity);
	RunE2E(TEXT("ConsoleRestart"), 128, false,
		ERealUdpFault::None, false, false, ERealUdpGateMode::ConsoleRestart);
	RunE2E(TEXT("Scale1000"), 1000, false, ERealUdpFault::None);
	RunE2E(TEXT("Scale10000"), 10000, false, ERealUdpFault::None);

	return true;
}

#endif
