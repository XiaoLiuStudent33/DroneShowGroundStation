#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "DSPGCSFrozenAssignmentProtocol.h"
#include "RouteAssignmentTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include <functional>
#include <vector>

#include "DSPGCSFrozenAssignmentSyncSubsystem.generated.h"

class UDSPGCSProductionTelemetryProviderSubsystem;
class UFleetManagerSubsystem;
class URouteAssignmentSubsystem;

UENUM(BlueprintType)
enum class EDSPGCSFrozenAssignmentSyncState : uint8
{
	NotFrozen,
	FrozenLocal,
	SyncingToConsole,
	ConsoleAccepted,
	SyncFailed
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FDSPGCSFrozenAssignmentSyncStatus
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	EDSPGCSFrozenAssignmentSyncState State = EDSPGCSFrozenAssignmentSyncState::NotFrozen;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	FString FleetSessionID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	FString AssignmentAuthoritySessionID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	int64 AssignmentRevision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	FString CanonicalPayloadDigest;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	FString TransactionID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	int32 AttemptCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	int32 MatchedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	int32 ChunkCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	int32 LastErrorCode = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	FString LastError;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Frozen Assignment")
	FDateTime AcceptedAtUtc;
};

/** Cached immutable wire transaction. Internal retries replay these exact frames. */
struct FDSPGCSFrozenAssignmentWireTransaction
{
	FString TransactionID;
	dspgcs::FrozenAssignmentIdentityDTO Identity;
	dspgcs::FrozenAssignmentDigest Digest{};
	std::uint32_t MatchedCount = 0;
	std::uint32_t ChunkCount = 0;
	std::size_t CanonicalPayloadBytes = 0;
	std::size_t EncodedTotalBytes = 0;
	std::vector<std::vector<std::uint8_t>> Frames;
};

struct FDSPGCSFrozenAssignmentPendingAck
{
	dspgcs::MessageEnvelope Envelope;
	dspgcs::FrozenAssignmentAckDTO Ack;
};

/**
 * Reliable transport projection for an already-frozen Route Assignment.
 * This subsystem never freezes, matches, or mutates assignment authority.
 */
UCLASS()
class DRONESHOWPLATFORM1_API UDSPGCSFrozenAssignmentSyncSubsystem final
	: public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Frozen Assignment")
	bool StartSyncFrozenAssignment(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Frozen Assignment")
	bool RetryFailedSync(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Frozen Assignment")
	bool CancelActiveSync(FString& OutError);

	UFUNCTION(BlueprintPure, Category = "DSP-GCS|Frozen Assignment")
	EDSPGCSFrozenAssignmentSyncState GetSyncState() const { return Status.State; }

	UFUNCTION(BlueprintPure, Category = "DSP-GCS|Frozen Assignment")
	FDSPGCSFrozenAssignmentSyncStatus GetSyncStatus() const { return Status; }

	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Frozen Assignment")
	void ProcessPendingAcks();

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FDSPGCSFrozenAssignmentSyncTests;
#endif

	using FFrameSender = std::function<bool(const std::vector<std::uint8_t>&)>;

	static bool BuildWireTransaction(
		const FRouteAssignmentSnapshot& Snapshot,
		const FString& TransactionID,
		std::uint64_t& InOutSequence,
		FDSPGCSFrozenAssignmentWireTransaction& OutTransaction,
		FString& OutError,
		std::size_t DatagramBudget =
			dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes);
	static FString DigestToHex(const dspgcs::FrozenAssignmentDigest& Digest);
	static bool IsRetryableError(dspgcs::FrozenAssignmentErrorCode ErrorCode);
	static bool IsSameBinding(
		const FDSPGCSFrozenAssignmentSyncStatus& StatusToCompare,
		const FDSPGCSFrozenAssignmentWireTransaction& Transaction);

	bool StartFromSnapshot(
		const FRouteAssignmentSnapshot& Snapshot,
		const FString& CurrentFleetSessionID,
		bool bRequireFailedSameAssignment,
		FString& OutError);
	bool SendActiveAttempt(double CurrentTimeSeconds);
	void ScheduleRetryOrFail(
		dspgcs::FrozenAssignmentErrorCode ErrorCode,
		const FString& SafeDiagnostic,
		double CurrentTimeSeconds);
	void FailSync(
		dspgcs::FrozenAssignmentErrorCode ErrorCode,
		const FString& SafeDiagnostic);
	void HandleDecodedAck(
		const dspgcs::MessageEnvelope& Envelope,
		const dspgcs::FrozenAssignmentAckDTO& Ack,
		double CurrentTimeSeconds);
	void EnqueueAck(
		const dspgcs::MessageEnvelope& Envelope,
		const dspgcs::FrozenAssignmentAckDTO& Ack);
	void RefreshFrozenBinding();
	void RefreshFromSnapshot(
		const FRouteAssignmentSnapshot* Snapshot,
		const FString& CurrentFleetSessionID);
	void AdvanceRetryStateAt(double CurrentTimeSeconds);
	bool TickSync(float DeltaSeconds);
	bool SendFrame(const std::vector<std::uint8_t>& Frame);
	FString MakeTransactionID() const;
	void BindStatusToTransaction(
		EDSPGCSFrozenAssignmentSyncState NewState,
		const FDSPGCSFrozenAssignmentWireTransaction& Transaction);
	void ResetToNotFrozen();

	TWeakObjectPtr<URouteAssignmentSubsystem> RouteAssignmentRef;
	TWeakObjectPtr<UFleetManagerSubsystem> FleetManagerRef;
	TWeakObjectPtr<UDSPGCSProductionTelemetryProviderSubsystem> TransportProviderRef;
	FDSPGCSFrozenAssignmentSyncStatus Status;
	FDSPGCSFrozenAssignmentWireTransaction ActiveTransaction;
	TQueue<FDSPGCSFrozenAssignmentPendingAck, EQueueMode::Mpsc> PendingAcks;
	FTSTicker::FDelegateHandle SyncTickerHandle;
	FFrameSender TestFrameSender;
	std::uint64_t OutgoingSequence = 0;
	double AckDeadlineSeconds = 0.0;
	double RetryAtSeconds = 0.0;
	double NextBindingRefreshSeconds = 0.0;
	bool bHasActiveTransaction = false;
	bool bWaitingForAck = false;
	bool bRetryScheduled = false;

	static constexpr int32 MaxAttempts = 3;
	static constexpr double AckTimeoutSeconds = 0.75;
	static constexpr double InitialRetryDelaySeconds = 0.25;
	static constexpr float SyncTickIntervalSeconds = 0.05f;
	static constexpr double BindingRefreshIntervalSeconds = 0.25;
};
