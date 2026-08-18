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

/** UE protocol client and read-only mirror; Console remains Job authority. */
UCLASS()
class DRONESHOWPLATFORM1_API UDSPGCSUploadJobSubsystem final
	: public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

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

	const FDSPGCSUploadJobStateMirror& GetStateMirror() const { return Mirror; }

private:
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
	bool TickClient(float DeltaSeconds);
	static std::string MakeCanonicalID();
	static bool ParseDigestHex(const FString& Hex, dspgcs::UploadJobDigest& OutDigest);

	TWeakObjectPtr<UDSPGCSProductionTelemetryProviderSubsystem> ProviderRef;
	FDSPGCSUploadJobStateMirror Mirror;
	TQueue<FDSPGCSUploadJobIncomingFrame, EQueueMode::Mpsc> IncomingFrames;
	FTSTicker::FDelegateHandle TickHandle;
	std::vector<std::uint8_t> PendingCommandFrame;
	std::vector<std::uint8_t> PendingSnapshotRequestFrame;
	std::string PendingSnapshotRequestID;
	std::string ActiveJobID;
	std::string ClientSessionID;
	std::uint64_t OutgoingSequence = 0;
	double RetryAtSeconds = 0.0;
	double SnapshotRetryAtSeconds = 0.0;
	double NextSnapshotPollSeconds = 0.0;
	int32 PendingAttemptCount = 0;
	int32 PendingSnapshotAttemptCount = 0;
	bool bSnapshotRequestInFlight = false;

	static constexpr int32 MaxCommandAttempts = 3;
	static constexpr int32 MaxSnapshotRequestAttempts = 3;
	static constexpr double CommandRetrySeconds = 0.5;
	static constexpr double SnapshotRequestRetrySeconds = 0.5;
	static constexpr double ValidationSnapshotPollSeconds = 0.5;
	static constexpr float ClientTickSeconds = 0.05f;
	static constexpr std::uint32_t RoutePageRequestMaximum = 20000;
};
