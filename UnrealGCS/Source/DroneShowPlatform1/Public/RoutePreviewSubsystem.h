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

	bool HasPreparedRoute() const { return PreparedRouteData.IsLoaded(); }
	int32 GetPreparedFrameCount() const { return PreparedRouteData.FrameCount; }
	int32 GetPreparedDroneCount() const { return PreparedRouteData.DroneCount; }

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
	int32 GetFrameCount() const { return RouteData.FrameCount; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	int32 GetDroneCount() const { return RouteData.DroneCount; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	int32 GetCurrentFrame() const { return CurrentFrame; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	bool IsRouteLoaded() const { return RouteData.IsLoaded(); }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	bool IsPreviewModeActive() const { return bPreviewModeActive; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	ERoutePreviewPlaybackState GetPlaybackState() const { return PlaybackState; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	double GetSampleRateHz() const { return RouteData.SampleRateHz; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	double GetCurrentTime() const { return CurrentTimeSeconds; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Preview")
	double GetDurationSeconds() const;

	const FRoutePreviewData& GetRouteData() const { return RouteData; }

private:
	FRoutePreviewData RouteData;
	FRoutePreviewData PreparedRouteData;
	FString PreparedRouteFilePath;
	int32 CurrentFrame = INDEX_NONE;
	double CurrentTimeSeconds = 0.0;
	ERoutePreviewPlaybackState PlaybackState = ERoutePreviewPlaybackState::Stopped;
	bool bPreviewModeActive = false;
	FTSTicker::FDelegateHandle PlaybackTickerHandle;

	/** Reused frame scratch buffers; SetPreviewFrame does not reparse the NPZ. */
	TArray<FVector3f> FramePositionsENU;
	TArray<FLinearColor> FrameColors;

	bool ApplyPreviewFrame(int32 FrameIndex, FString& OutError);
	bool TickPlayback(float DeltaSeconds);
	void StartPlaybackTicker();
	void StopPlaybackTicker();
	class ADroneFleetVisualizationActor* FindVisualizationActor() const;
};
