#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RouteGpsCaptureSubsystem.generated.h"

UENUM(BlueprintType)
enum class ERouteGpsCaptureState : uint8
{
	Idle,
	Capturing,
	Ready,
	Failed,
	Invalidated
};

UENUM(BlueprintType)
enum class ERouteGpsCaptureParticipantStatus : uint8
{
	Pending,
	Ready,
	Failed
};

UENUM(BlueprintType)
enum class ERouteGpsCaptureFailureReason : uint8
{
	None,
	Offline,
	ParticipantMissing,
	EndpointChanged,
	FleetSessionChanged,
	GpsUnavailable,
	GpsStale,
	FixQualityInsufficient,
	InsufficientSamples,
	UnstablePosition,
	SelectionInvalidated,
	Superseded
};

/**
 * Explicit caller-owned policy. Zero defaults are deliberately invalid so F.1
 * does not freeze uncalibrated production thresholds before outdoor testing.
 */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteGpsCapturePolicy
{
	GENERATED_BODY()

	/** Human-readable configuration identity; it is part of the capture audit record. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	FString ProfileName;

	/** False profiles are permitted for Editor/simulation commissioning only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	bool bHardwareValidated = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	int32 RequiredFixType = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	int64 MaxSampleAgeMilliseconds = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	int64 MinWindowDurationMilliseconds = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	int32 MinUniqueSamples = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	double MaxMedianResidualMeters = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	double MaxRobustSpreadMeters = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	double MaxSampleResidualMeters = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	double MaxJumpDistanceMeters = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route GPS Capture")
	int64 CaptureTimeoutMilliseconds = 0;
};

/** One DSP telemetry observation. GpsSampleTimestampUtcMs is Console receive time. */
struct DRONESHOWPLATFORM1_API FRouteGpsTelemetryObservation
{
	FString CaptureRequestID;
	FString DroneID;
	FString EndpointIdentity;
	bool bVehicleOnline = false;
	bool bGpsValid = false;
	int32 FixType = 0;
	double Latitude = 0.0;
	double Longitude = 0.0;
	float AltitudeAMSL = 0.0f;
	bool bAltitudeValid = false;
	int64 GpsSampleTimestampUtcMs = 0;
	int64 TelemetryReceivedUtcMs = 0;
	double TelemetryReceivedMonotonicSeconds = 0.0;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteGpsCaptureParticipantResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FString EndpointIdentity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	int32 FixType = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	int64 FirstGpsSampleUtcMs = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	int64 LastGpsSampleUtcMs = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	int32 UniqueSampleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	double EstimatedLatitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	double EstimatedLongitude = 0.0;

	/** Audit-only. It is not used by the horizontal estimator or F.1 matching. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	float AltitudeAMSL = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	double MedianResidualMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	double RobustSpreadMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	double MaxResidualMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	ERouteGpsCaptureParticipantStatus Status = ERouteGpsCaptureParticipantStatus::Pending;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	ERouteGpsCaptureFailureReason FailureReason = ERouteGpsCaptureFailureReason::None;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteGpsCaptureSnapshot
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FString CaptureAuthoritySessionID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FString CaptureRequestID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	int64 CaptureRevision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FString SelectionDigest;

	/** True only for the legacy F.1 Command Selection capture lane. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	bool bCommandSelectionBound = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FString FleetSessionID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	TArray<FString> SelectedDroneIDs;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FRouteGpsCapturePolicy Policy;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	ERouteGpsCaptureState State = ERouteGpsCaptureState::Idle;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FDateTime StartedAtUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	FDateTime CompletedAtUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route GPS Capture")
	TArray<FRouteGpsCaptureParticipantResult> Participants;
};

/**
 * Session-scoped authority for selection-bound, endpoint-bound stable GPS
 * capture. It does not perform RouteSlot matching or mutate upload authority.
 */
UCLASS()
class DRONESHOWPLATFORM1_API URouteGpsCaptureSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool BeginCapture(const FRouteGpsCapturePolicy& Policy, FString& OutError);
	/**
	 * Starts the same F.1 capture algorithm for an explicit participant set
	 * without reading or mutating Command Selection.
	 */
	bool BeginCaptureForParticipants(
		const FRouteGpsCapturePolicy& Policy,
		const TArray<FString>& ParticipantDroneIDs,
		FString& OutError);
	static bool LoadConfiguredPolicy(FRouteGpsCapturePolicy& OutPolicy, FString& OutError);
	void CancelCapture();
	void ObserveTelemetrySample(const FRouteGpsTelemetryObservation& Observation);
	bool GetCurrentSnapshot(FRouteGpsCaptureSnapshot& OutSnapshot) const;
	/** Returns the newest Command Selection-bound snapshot, excluding explicit captures. */
	bool GetLatestCommandSelectionSnapshot(FRouteGpsCaptureSnapshot& OutSnapshot) const;
	ERouteGpsCaptureState GetState() const { return CurrentSnapshot.State; }
	const FString& GetCurrentCaptureRequestID() const { return CurrentSnapshot.CaptureRequestID; }

#if WITH_DEV_AUTOMATION_TESTS
	bool BeginCaptureForSelectionForTest(
		const FRouteGpsCapturePolicy& Policy,
		const TArray<FString>& SelectedDroneIDs,
		FString& OutError);
	void EvaluateAtForTest(int64 NowUtcMs, double NowMonotonicSeconds);
#endif

private:
	struct FUniqueSample
	{
		int64 GpsTimestampUtcMs = 0;
		double Latitude = 0.0;
		double Longitude = 0.0;
		float AltitudeAMSL = 0.0f;
		bool bAltitudeValid = false;
		int32 FixType = 0;
		int64 FirstObservedUtcMs = 0;
		double FirstObservedMonotonicSeconds = 0.0;
	};

	struct FParticipantWindow
	{
		FString EndpointIdentity;
		TArray<FUniqueSample> Samples;
	};

	FString CaptureAuthoritySessionID;
	int64 LastIssuedCaptureRevision = 0;
	FRouteGpsCaptureSnapshot CurrentSnapshot;
	FRouteGpsCaptureSnapshot LastCommandSelectionSnapshot;
	TMap<FString, FParticipantWindow> WindowsByDroneID;
	double CaptureStartedMonotonicSeconds = 0.0;
	FDelegateHandle SelectionChangedHandle;
	FTSTicker::FDelegateHandle FreshnessTickerHandle;

	bool BeginCaptureInternal(
		const FRouteGpsCapturePolicy& Policy,
		TArray<FString> SelectedDroneIDs,
		bool bCommandSelectionBound,
		FString& OutError);
	bool EvaluateParticipant(
		FRouteGpsCaptureParticipantResult& Participant,
		const FParticipantWindow& Window,
		int64 NowUtcMs,
		double NowMonotonicSeconds,
		ERouteGpsCaptureFailureReason& OutFailureReason) const;
	void EvaluateCapture(int64 NowUtcMs, double NowMonotonicSeconds);
	void InvalidateCapture(ERouteGpsCaptureFailureReason Reason);
	void HandleSelectionChanged();
	UFUNCTION()
	void HandleFleetDataApplied(int64 FleetRevision, int32 UpdatedDroneCount);
	bool TickFreshness(float DeltaSeconds);

	static bool ValidatePolicy(const FRouteGpsCapturePolicy& Policy, FString& OutError);
	static bool ComputeSelectionDigest(
		const TArray<FString>& SelectedDroneIDs,
		FString& OutDigest,
		FString& OutError);
	static int64 UtcNowMilliseconds();
};
