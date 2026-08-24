#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "RoutePreviewData.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RoutePreviewSubsystem.generated.h"

UENUM(BlueprintType)
enum class ERoutePreviewPlaybackState : uint8
{
	Stopped,
	Playing,
	Paused,
	Completed
};

/**
 * Route preview authority. Route slots remain preview-only identities and all
 * frame visualization is submitted through the existing batched Niagara path.
 */
UCLASS()
class DRONESHOWPLATFORM1_API URoutePreviewSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool LoadRoute(const FString& FilePath, FString& OutError);

	/** Loads and validates an import candidate without changing the active route. */
	bool PrepareRouteImport(const FString& FilePath, FString& OutError);

	/** Replaces the active route only after the import UI has validated its metadata. */
	bool CommitPreparedRoute(FString& OutError);

	/**
	 * Validates the prepared route, Show Origin and Frame-0 overlay as one
	 * candidate, then atomically replaces the active Route Dataset.
	 */
	bool CommitPreparedRouteForWorkflow(
		double Latitude,
		double Longitude,
		double Altitude,
		bool bEnableOverlay,
		FString& OutError);

	bool HasPreparedRoute() const { return PreparedRouteData.IsLoaded(); }
	int32 GetPreparedFrameCount() const { return PreparedRouteData.FrameCount; }
	int32 GetPreparedDroneCount() const { return PreparedRouteData.DroneCount; }
	const FRouteArtifactSourceIdentity& GetPreparedSourceIdentity() const
	{
		return PreparedRouteData.SourceIdentity;
	}

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	void UnloadRoute();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool SetShowOrigin(double Latitude, double Longitude, double Altitude, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool EnterPreviewMode(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	void ExitPreviewMode();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool SetPreviewFrame(int32 FrameIndex, FString& OutError);

	/** Manual seek authority used by console and UI. Pauses active playback. */
	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool SeekToFrame(int32 FrameIndex, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool SetSampleRateHz(double Hz, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool Play(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	void Pause();

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Preview")
	bool Stop(FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	int32 GetFrameCount() const { return GetActiveRouteData().FrameCount; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	int32 GetDroneCount() const { return GetActiveRouteData().DroneCount; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	int32 GetCurrentFrame() const { return CurrentFrame; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	bool IsRouteLoaded() const { return GetActiveRouteData().IsLoaded(); }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	bool IsPreviewModeActive() const { return bPreviewModeActive; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	ERoutePreviewPlaybackState GetPlaybackState() const { return PlaybackState; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	double GetSampleRateHz() const { return GetActiveRouteData().SampleRateHz; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	double GetCurrentTime() const { return CurrentTimeSeconds; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	double GetDurationSeconds() const;

	/** Enables/disables the independent Live + Route Frame-0 target overlay preference. */
	bool SetRouteOverlayEnabled(bool bEnabled, FString& OutError);

	bool IsRouteOverlayEnabled() const { return bRouteOverlayEnabled; }
	bool IsRouteOverlayVisible() const { return bRouteOverlayVisible; }

	/** Rebuilds the O(DroneCount) Frame-0 world-position cache and submits it once. */
	bool RefreshRouteOverlay(FString& OutError);

	int32 GetCachedRouteTargetCount() const { return CachedRouteTargetWorldPositions.Num(); }
	void LogRouteOverlayStatus() const;

	const FRoutePreviewData& GetRouteData() const { return GetActiveRouteData(); }
	/** Captures the active dataset lifetime without copying PositionsENU or Colors. */
	TSharedPtr<const FRoutePreviewData, ESPMode::ThreadSafe> CaptureRouteDataSnapshot() const;
	const FRouteArtifactSourceIdentity& GetActiveSourceIdentity() const
	{
		return GetActiveRouteData().SourceIdentity;
	}
	int64 GetRouteDatasetGeneration() const { return RouteDatasetGeneration; }

private:
	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> ActiveRouteData;
	FRoutePreviewData PreparedRouteData;
	FString PreparedRouteFilePath;
	int64 RouteDatasetGeneration = 0;
	int32 CurrentFrame = INDEX_NONE;
	double CurrentTimeSeconds = 0.0;
	ERoutePreviewPlaybackState PlaybackState = ERoutePreviewPlaybackState::Stopped;
	bool bPreviewModeActive = false;
	bool bRouteOverlayEnabled = false;
	bool bRouteOverlayVisible = false;
	FTSTicker::FDelegateHandle PlaybackTickerHandle;

	/** Reused frame scratch buffers; SetPreviewFrame does not reparse the NPZ. */
	TArray<FVector3f> FramePositionsENU;
	TArray<FLinearColor> FrameColors;

	/** Frame-0 only, ordered exactly by RouteSlotIndex; never a Fleet identity mapping. */
	TArray<FVector> CachedRouteTargetWorldPositions;

	bool ApplyPreviewFrame(int32 FrameIndex, FString& OutError);
	bool TickPlayback(float DeltaSeconds);
	void StartPlaybackTicker();
	void StopPlaybackTicker();
	void ClearRouteOverlayRenderData();
	class ADroneFleetVisualizationActor* FindVisualizationActor() const;
	const FRoutePreviewData& GetActiveRouteData() const;
	FRoutePreviewData& GetMutableActiveRouteData();
};
