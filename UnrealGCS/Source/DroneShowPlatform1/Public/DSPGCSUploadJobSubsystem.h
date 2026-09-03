#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "DSPGCSUploadJobProtocol.h"
#include "DSPGCSUploadJobStateMirror.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include <vector>

#include "DSPGCSUploadJobSubsystem.generated.h"

class UDSPGCSProductionTelemetryProviderSubsystem;
class UDSPGCSFrozenAssignmentSyncSubsystem;
class URouteArtifactWorkspaceSubsystem;

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FDSPGCSUploadJobCommandStatus
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bCommandInFlight = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bCommandTimedOut = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bHasCommandAck = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bHasActiveJobBinding = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bHasExecutableJob = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bHasAuthoritativeFailure = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bRecoveryRequired = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 CurrentCommandType = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 LastCommandAckStatus = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 LastCommandResultCode = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	FString ActiveUploadJobID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	FString LastSafeDiagnostic;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FDSPGCSUploadJobMirrorStatus
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bHasAuthoritativeSnapshot = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	bool bRecoveryRequired = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	FString UploadJobID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 JobState = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int64 JobRevision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 Matched = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 Waiting = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 Preparing = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 Uploading = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 Succeeded = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 Failed = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int64 TotalBytes = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int64 BytesCompleted = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int64 ThroughputBytesPerSecond = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 EffectiveMaxConcurrentUploads = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DSP-GCS|Upload Job")
	int32 MirroredRouteCount = 0;
};

struct FDSPGCSUploadJobIncomingFrame
{
	dspgcs::MessageEnvelope Envelope;
	std::vector<std::uint8_t> Payload;
};

enum class EDSPGCSMavFtpRuntimeConfigState : uint8
{
	Loading,
	Connected,
	Disconnected,
	Stale
};

struct FDSPGCSMavFtpRuntimeConfigStatus
{
	EDSPGCSMavFtpRuntimeConfigState State =
		EDSPGCSMavFtpRuntimeConfigState::Loading;
	bool bHasSnapshot = false;
	bool bRequestInFlight = false;
	bool bLastApplyAccepted = false;
	bool bLastApplyRejected = false;
	bool bLastAckDuplicate = false;
	FString LastSafeDiagnostic;
};

/** UE protocol client and read-only mirror; Console remains Job authority. */
UCLASS()
class DRONESHOWPLATFORM1_API UDSPGCSUploadJobSubsystem final
	: public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Captures Current Artifact and ConsoleAccepted Frozen identities internally. */
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool StartUploadJobFromCurrentAuthorities(
		int32 RequestedMaxConcurrentUploads,
		FString& OutUploadJobID,
		FString& OutError);
	bool StartUploadJobFromCurrentAuthorities(
		int32 RequestedMaxConcurrentUploads,
		const dspgcs::UploadRuntimeConfigBindingDTO& ExpectedRuntimeConfig,
		FString& OutUploadJobID,
		FString& OutError);

	bool RequestMavFtpRuntimeConfigSnapshot(FString& OutError);
	bool ApplyMavFtpRuntimeGlobalCap(int32 CandidateCap, FString& OutError);
	bool CaptureMavFtpRuntimeConfigBinding(
		dspgcs::UploadRuntimeConfigBindingDTO& OutBinding,
		FString& OutError) const;
	const dspgcs::MavFtpRuntimeConfigSnapshotDTO*
		GetMavFtpRuntimeConfigSnapshot() const
		{ return bHasRuntimeConfigSnapshot ? &RuntimeConfigSnapshot : nullptr; }
	FDSPGCSMavFtpRuntimeConfigStatus GetMavFtpRuntimeConfigStatus() const;

	/** Explicit terminal/error-to-NoJob transition. Never attaches an unknown Console Job. */
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool PrepareForNewUploadJob(FString& OutError);

	/** Read-only preflight used by transactional Route Dataset replacement. */
	bool CanPrepareForNewUploadJob(FString& OutError) const;

	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool StartUploadJob(
		const FString& RequestedUploadJobID,
		const FString& ArtifactSetID,
		const FString& ArtifactSetDigestHex,
		const FString& FleetSessionID,
		const FString& AssignmentAuthoritySessionID,
		int64 AssignmentRevision,
		const FString& AssignmentDigestHex,
		int32 RequestedMaxConcurrentUploads,
		FString& OutUploadJobID,
		FString& OutError);
	bool StartUploadJobWithRuntimeConfig(
		const FString& RequestedUploadJobID,
		const FString& ArtifactSetID,
		const FString& ArtifactSetDigestHex,
		const FString& FleetSessionID,
		const FString& AssignmentAuthoritySessionID,
		int64 AssignmentRevision,
		const FString& AssignmentDigestHex,
		const dspgcs::UploadRuntimeConfigBindingDTO& RuntimeConfigBinding,
		int32 RequestedMaxConcurrentUploads,
		FString& OutUploadJobID,
		FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool PauseUploadJob(FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool ResumeUploadJob(FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool CancelUploadJob(FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool RetryFailedRoutes(FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool SetMaxConcurrentUploads(int32 Maximum, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	bool RequestAuthoritativeSnapshot(FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "DSP-GCS|Upload Job")
	void NotifyUploadPageOpened();

	UFUNCTION(BlueprintPure, Category = "DSP-GCS|Upload Job")
	FDSPGCSUploadJobMirrorStatus GetMirrorStatus() const;
	UFUNCTION(BlueprintPure, Category = "DSP-GCS|Upload Job")
	FDSPGCSUploadJobCommandStatus GetCommandStatus() const;

	const FDSPGCSUploadJobStateMirror& GetStateMirror() const { return Mirror; }

#if WITH_DEV_AUTOMATION_TESTS
	void SetMavFtpRuntimeConfigForTest(
		const dspgcs::MavFtpRuntimeConfigSnapshotDTO& Snapshot,
		EDSPGCSMavFtpRuntimeConfigState State =
			EDSPGCSMavFtpRuntimeConfigState::Connected)
	{
		RuntimeConfigSnapshot = Snapshot;
		bHasRuntimeConfigSnapshot = true;
		RuntimeConfigState = State;
		bRuntimeConfigRequestInFlight = false;
	}
	void BindUploadJobForTest(const std::string& UploadJobID)
	{
		ActiveJobID = UploadJobID;
		Mirror.PrepareForNewJob(UploadJobID);
	}
	bool ApplySnapshotForTest(const dspgcs::UploadJobSnapshotDTO& Snapshot)
	{
		return Mirror.ApplySnapshot(Snapshot);
	}
	bool ApplyRoutePageForTest(const dspgcs::UploadJobRouteStatePageDTO& Page)
	{
		return Mirror.ApplyRoutePage(Page);
	}
#endif

private:
	bool CaptureCurrentAuthorityBindings(
		dspgcs::UploadArtifactBindingDTO& OutArtifactBinding,
		dspgcs::UploadFrozenAssignmentBindingDTO& OutFrozenBinding,
		FString& OutError);
	bool SendMavFtpRuntimeConfigRequest(
		dspgcs::MavFtpRuntimeConfigRequestDTO Request,
		FString& OutError);
	void ClearPendingMavFtpRuntimeConfigRequest();
	bool SendSimpleCommand(
		dspgcs::UploadJobCommandType CommandType,
		std::uint32_t RequestedConcurrency,
		FString& OutError);
	bool SendCommand(dspgcs::UploadJobCommandDTO Command, FString& OutError);
	bool SendSnapshotRequest(
		dspgcs::UploadJobSnapshotRequestKind Kind,
		std::uint32_t PageStart,
		const std::string& SnapshotToken,
		FString& OutError);
	bool SendPayload(
		dspgcs::MessageType MessageType,
		const std::string& CorrelationID,
		const std::vector<std::uint8_t>& Payload,
		std::vector<std::uint8_t>* OutFrame,
		FString& OutError);
	void EnqueueIncoming(
		const dspgcs::MessageEnvelope& Envelope,
		const std::vector<std::uint8_t>& Payload);
	void ProcessIncoming();
	void ClearPendingSnapshotRequest();
	void SetLocalCommandFailure(
		dspgcs::UploadJobCommandType CommandType,
		const FString& SafeDiagnostic);
	bool TickClient(float DeltaSeconds);
	static std::string MakeCanonicalID();
	static bool ParseDigestHex(const FString& Hex, dspgcs::UploadJobDigest& OutDigest);

	TWeakObjectPtr<UDSPGCSProductionTelemetryProviderSubsystem> ProviderRef;
	TWeakObjectPtr<URouteArtifactWorkspaceSubsystem> WorkspaceRef;
	TWeakObjectPtr<UDSPGCSFrozenAssignmentSyncSubsystem> FrozenSyncRef;
	FDSPGCSUploadJobStateMirror Mirror;
	TQueue<FDSPGCSUploadJobIncomingFrame, EQueueMode::Mpsc> IncomingFrames;
	FTSTicker::FDelegateHandle TickHandle;
	std::vector<std::uint8_t> PendingCommandFrame;
	std::vector<std::uint8_t> PendingSnapshotRequestFrame;
	std::vector<std::uint8_t> PendingRuntimeConfigRequestFrame;
	std::string PendingRuntimeConfigRequestID;
	std::string PendingRuntimeConfigExpectedAuthoritySessionID;
	dspgcs::MavFtpRuntimeConfigOperation PendingRuntimeConfigOperation =
		dspgcs::MavFtpRuntimeConfigOperation::Get;
	dspgcs::MavFtpRuntimeConfigSnapshotDTO RuntimeConfigSnapshot;
	std::string PendingSnapshotRequestID;
	dspgcs::UploadJobSnapshotRequestKind PendingSnapshotRequestKind =
		dspgcs::UploadJobSnapshotRequestKind::Aggregate;
	std::string ActiveJobID;
	std::string ClientSessionID;
	std::uint64_t OutgoingSequence = 0;
	double RetryAtSeconds = 0.0;
	double SnapshotRetryAtSeconds = 0.0;
	double NextSnapshotPollSeconds = 0.0;
	int32 PendingAttemptCount = 0;
	int32 PendingSnapshotAttemptCount = 0;
	int32 TerminalAggregateRefreshAttemptCount = 0;
	int32 PendingRuntimeConfigAttemptCount = 0;
	double RuntimeConfigRetryAtSeconds = 0.0;
	bool bRuntimeConfigRequestInFlight = false;
	bool bHasRuntimeConfigSnapshot = false;
	bool bLastRuntimeConfigApplyAccepted = false;
	bool bLastRuntimeConfigApplyRejected = false;
	bool bLastRuntimeConfigAckDuplicate = false;
	FString LastRuntimeConfigDiagnostic;
	EDSPGCSMavFtpRuntimeConfigState RuntimeConfigState =
		EDSPGCSMavFtpRuntimeConfigState::Loading;
	bool bSnapshotRequestInFlight = false;
	bool bAggregateRefreshRequired = false;
	bool bCommandInFlight = false;
	bool bCommandTimedOut = false;
	bool bHasCommandAck = false;
	dspgcs::UploadJobCommandType CurrentCommandType =
		dspgcs::UploadJobCommandType::Start;
	dspgcs::UploadJobCommandAckStatus LastCommandAckStatus =
		dspgcs::UploadJobCommandAckStatus::Rejected;
	dspgcs::UploadJobErrorCode LastCommandResultCode =
		dspgcs::UploadJobErrorCode::None;
	FString LastSafeDiagnostic;

#if WITH_DEV_AUTOMATION_TESTS
	friend class FDSPGCSUploadJobClientHardeningTest;
	friend class FGCSRouteWorkflowRouteListSnapshotLifecycleTest;
	friend class FGCSRouteWorkflowPreJobPresentationTest;
	TFunction<bool(
		dspgcs::UploadArtifactBindingDTO&,
		dspgcs::UploadFrozenAssignmentBindingDTO&,
		FString&)> StartAuthorityCaptureForTest;
	TFunction<bool(const std::vector<std::uint8_t>&)> FrameSenderForTest;
#endif

	static constexpr int32 MaxCommandAttempts = 3;
	static constexpr int32 MaxSnapshotRequestAttempts = 3;
	static constexpr double CommandRetrySeconds = 0.5;
    static constexpr double SnapshotRequestRetrySeconds = 0.5;
    static constexpr double LiveSnapshotPollSeconds = 0.25;
    static constexpr float ClientTickSeconds = 0.05f;
	static constexpr std::uint32_t RoutePageRequestMaximum = 20000;
};
