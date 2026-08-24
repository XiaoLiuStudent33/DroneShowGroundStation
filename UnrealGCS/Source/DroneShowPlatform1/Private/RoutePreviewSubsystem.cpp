#include "RoutePreviewSubsystem.h"

#include "DroneFleetVisualizationActor.h"
#include "Containers/Ticker.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "RoutePreviewNpzLoader.h"

void URoutePreviewSubsystem::Deinitialize()
{
	StopPlaybackTicker();
	ExitPreviewMode();
	ClearRouteOverlayRenderData();
	ActiveRouteData.Reset();
	PreparedRouteData = FRoutePreviewData{};
	PreparedRouteFilePath.Reset();
	FramePositionsENU.Reset();
	FrameColors.Reset();
	CachedRouteTargetWorldPositions.Reset();
	Super::Deinitialize();
}

bool URoutePreviewSubsystem::PrepareRouteImport(const FString& FilePath, FString& OutError)
{
	FRoutePreviewData CandidateData;
	if (!FRoutePreviewNpzLoader::Load(FilePath, CandidateData, OutError))
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreview] Import validation failed: %s"), *OutError);
		return false;
	}

	PreparedRouteData = MoveTemp(CandidateData);
	PreparedRouteFilePath = FilePath;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RoutePreview] Import prepared Path=%s Frames=%d Drones=%d"),
		*PreparedRouteFilePath,
		PreparedRouteData.FrameCount,
		PreparedRouteData.DroneCount);
	return true;
}

bool URoutePreviewSubsystem::CommitPreparedRoute(FString& OutError)
{
	OutError.Reset();
	if (!PreparedRouteData.IsLoaded())
	{
		OutError = TEXT("No validated route import is prepared.");
		return false;
	}

	StopPlaybackTicker();
	PlaybackState = ERoutePreviewPlaybackState::Stopped;
	CurrentTimeSeconds = 0.0;
	if (bPreviewModeActive)
	{
		ExitPreviewMode();
	}

	ActiveRouteData = MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>(MoveTemp(PreparedRouteData));
	PreparedRouteData = FRoutePreviewData{};
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	++RouteDatasetGeneration;
	// A committed import starts at logical frame zero while remaining in Live
	// visualization. Full preview ownership begins only after an explicit
	// playback/seek action.
	CurrentFrame = 0;
	FramePositionsENU.SetNumUninitialized(RouteData.DroneCount);
	FrameColors.SetNumUninitialized(RouteData.DroneCount);
	ClearRouteOverlayRenderData();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RoutePreview] Import committed Path=%s Frames=%d Drones=%d"),
		*PreparedRouteFilePath,
		RouteData.FrameCount,
		RouteData.DroneCount);
	PreparedRouteFilePath.Reset();
	if (bRouteOverlayEnabled)
	{
		FString OverlayError;
		RefreshRouteOverlay(OverlayError);
	}
	return true;
}

bool URoutePreviewSubsystem::CommitPreparedRouteForWorkflow(
	const double Latitude,
	const double Longitude,
	const double Altitude,
	const bool bEnableOverlay,
	FString& OutError)
{
	OutError.Reset();
	if (!PreparedRouteData.IsLoaded())
	{
		OutError = TEXT("No validated route import is prepared.");
		return false;
	}
	if (!FMath::IsFinite(Latitude) || Latitude < -90.0 || Latitude > 90.0)
	{
		OutError = TEXT("Latitude must be finite and in [-90,90].");
		return false;
	}
	if (!FMath::IsFinite(Longitude) || Longitude < -180.0 || Longitude > 180.0)
	{
		OutError = TEXT("Longitude must be finite and in [-180,180].");
		return false;
	}
	if (!FMath::IsFinite(Altitude))
	{
		OutError = TEXT("Altitude must be finite.");
		return false;
	}

	ADroneFleetVisualizationActor* VisualizationActor = nullptr;
	TArray<FVector> CandidateWorldPositions;
	if (bEnableOverlay)
	{
		VisualizationActor = FindVisualizationActor();
		if (!VisualizationActor)
		{
			OutError = TEXT("DroneFleetVisualizationActor is unavailable in the current world.");
			return false;
		}
		TArray<FVector3f> CandidateFramePositions;
		CandidateFramePositions.SetNumUninitialized(PreparedRouteData.DroneCount);
		for (int32 RouteSlotIndex = 0;
			RouteSlotIndex < PreparedRouteData.DroneCount;
			++RouteSlotIndex)
		{
			CandidateFramePositions[RouteSlotIndex] =
				PreparedRouteData.GetPosition(0, RouteSlotIndex);
		}
		FVector CandidateOriginWorld = FVector::ZeroVector;
		if (!VisualizationActor->ConvertRouteENUPositionsToWorld(
			CandidateFramePositions,
			Latitude,
			Longitude,
			Altitude,
			CandidateWorldPositions,
			CandidateOriginWorld,
			OutError))
		{
			return false;
		}
		if (CandidateWorldPositions.Num() != PreparedRouteData.DroneCount)
		{
			OutError = TEXT("Route overlay conversion returned an unexpected target count.");
			return false;
		}
	}

	// Nothing above this line changes the active Dataset or render state.
	PreparedRouteData.ShowOriginLatitude = Latitude;
	PreparedRouteData.ShowOriginLongitude = Longitude;
	PreparedRouteData.ShowOriginAltitude = Altitude;
	PreparedRouteData.bShowOriginSet = true;
	StopPlaybackTicker();
	PlaybackState = ERoutePreviewPlaybackState::Stopped;
	CurrentTimeSeconds = 0.0;
	if (bPreviewModeActive)
	{
		ExitPreviewMode();
	}

	ActiveRouteData = MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>(
		MoveTemp(PreparedRouteData));
	PreparedRouteData = FRoutePreviewData{};
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	++RouteDatasetGeneration;
	CurrentFrame = 0;
	FramePositionsENU.SetNumUninitialized(RouteData.DroneCount);
	FrameColors.SetNumUninitialized(RouteData.DroneCount);
	ClearRouteOverlayRenderData();
	bRouteOverlayEnabled = bEnableOverlay;
	if (bEnableOverlay)
	{
		CachedRouteTargetWorldPositions = MoveTemp(CandidateWorldPositions);
		VisualizationActor->SetRouteTargetPositions(CachedRouteTargetWorldPositions);
		const bool bCanShow =
			VisualizationActor->GetVisualizationMode() == EGCSVisualizationMode::Live;
		VisualizationActor->SetRouteTargetsVisible(bCanShow);
		bRouteOverlayVisible = bCanShow && VisualizationActor->AreRouteTargetsVisible();
	}
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RoutePreview] Workflow import committed Path=%s Frames=%d Drones=%d Overlay=%s"),
		*PreparedRouteFilePath,
		RouteData.FrameCount,
		RouteData.DroneCount,
		bRouteOverlayVisible ? TEXT("Visible") : TEXT("Hidden"));
	PreparedRouteFilePath.Reset();
	return true;
}

bool URoutePreviewSubsystem::LoadRoute(const FString& FilePath, FString& OutError)
{
	OutError.Reset();
	StopPlaybackTicker();
	PlaybackState = ERoutePreviewPlaybackState::Stopped;
	CurrentTimeSeconds = 0.0;
	if (bPreviewModeActive)
	{
		ExitPreviewMode();
	}

	FRoutePreviewData LoadedData;
	const double StartSeconds = FPlatformTime::Seconds();
	if (!FRoutePreviewNpzLoader::Load(FilePath, LoadedData, OutError))
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreview] Load failed: %s"), *OutError);
		return false;
	}

	ActiveRouteData = MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>(MoveTemp(LoadedData));
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	++RouteDatasetGeneration;
	CurrentFrame = INDEX_NONE;
	FramePositionsENU.SetNumUninitialized(RouteData.DroneCount);
	FrameColors.SetNumUninitialized(RouteData.DroneCount);
	ClearRouteOverlayRenderData();
	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RoutePreview] Loaded Path=%s Frames=%d Drones=%d Range=[%d,%d] Position=%.2fMiB Color=%.2fMiB Total=%.2fMiB SampleRate=UNKNOWN Elapsed=%.3fs"),
		*FilePath,
		RouteData.FrameCount,
		RouteData.DroneCount,
		RouteData.OptionalRangeStart,
		RouteData.OptionalRangeEnd,
		RouteData.GetPositionMemoryBytes() / (1024.0 * 1024.0),
		RouteData.GetColorMemoryBytes() / (1024.0 * 1024.0),
		(RouteData.GetPositionMemoryBytes() + RouteData.GetColorMemoryBytes()) / (1024.0 * 1024.0),
		ElapsedSeconds);
	if (bRouteOverlayEnabled)
	{
		FString OverlayError;
		RefreshRouteOverlay(OverlayError);
	}
	return true;
}

void URoutePreviewSubsystem::UnloadRoute()
{
	const bool bHadActiveRouteDataset = GetActiveRouteData().IsLoaded();
	StopPlaybackTicker();
	ExitPreviewMode();
	ClearRouteOverlayRenderData();
	ActiveRouteData.Reset();
	if (bHadActiveRouteDataset)
	{
		++RouteDatasetGeneration;
	}
	CurrentFrame = INDEX_NONE;
	CurrentTimeSeconds = 0.0;
	PlaybackState = ERoutePreviewPlaybackState::Stopped;
	FramePositionsENU.Reset();
	FrameColors.Reset();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RoutePreview] Unloaded; route overlay cleared (EnabledPreference=%s)"),
		bRouteOverlayEnabled ? TEXT("YES") : TEXT("NO"));
}

bool URoutePreviewSubsystem::SetShowOrigin(
	const double Latitude,
	const double Longitude,
	const double Altitude,
	FString& OutError)
{
	FRoutePreviewData& RouteData = GetMutableActiveRouteData();
	OutError.Reset();
	if (!FMath::IsFinite(Latitude) || Latitude < -90.0 || Latitude > 90.0)
	{
		OutError = TEXT("Latitude must be finite and in [-90,90].");
		return false;
	}
	if (!FMath::IsFinite(Longitude) || Longitude < -180.0 || Longitude > 180.0)
	{
		OutError = TEXT("Longitude must be finite and in [-180,180].");
		return false;
	}
	if (!FMath::IsFinite(Altitude))
	{
		OutError = TEXT("Altitude must be finite.");
		return false;
	}

	RouteData.ShowOriginLatitude = Latitude;
	RouteData.ShowOriginLongitude = Longitude;
	RouteData.ShowOriginAltitude = Altitude;
	RouteData.bShowOriginSet = true;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RoutePreview] ShowOrigin Lat=%.9f Lon=%.9f Alt=%.3fm"),
		Latitude,
		Longitude,
		Altitude);
	if (bRouteOverlayEnabled)
	{
		FString OverlayError;
		RefreshRouteOverlay(OverlayError);
	}
	return true;
}

bool URoutePreviewSubsystem::EnterPreviewMode(FString& OutError)
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	OutError.Reset();
	if (!RouteData.IsLoaded())
	{
		OutError = TEXT("No route is loaded.");
		return false;
	}
	if (!RouteData.bShowOriginSet)
	{
		OutError = TEXT("Route preview ShowOrigin is not set.");
		return false;
	}
	if (!FindVisualizationActor())
	{
		OutError = TEXT("DroneFleetVisualizationActor is unavailable in the current world.");
		return false;
	}

	bPreviewModeActive = true;
	if (ADroneFleetVisualizationActor* VisualizationActor = FindVisualizationActor())
	{
		const bool bWasVisible = bRouteOverlayVisible || VisualizationActor->AreRouteTargetsVisible();
		VisualizationActor->SetRouteTargetsVisible(false);
		bRouteOverlayVisible = false;
		if (bWasVisible)
		{
			UE_LOG(LogTemp, Display, TEXT("[RouteOverlay] Hidden while full RoutePreview is active"));
		}
	}
	return true;
}

void URoutePreviewSubsystem::ExitPreviewMode()
{
	StopPlaybackTicker();
	PlaybackState = ERoutePreviewPlaybackState::Stopped;
	CurrentTimeSeconds = 0.0;
	if (!bPreviewModeActive)
	{
		return;
	}
	if (ADroneFleetVisualizationActor* VisualizationActor = FindVisualizationActor())
	{
		VisualizationActor->ExitRoutePreview();
	}
	bPreviewModeActive = false;
	CurrentFrame = INDEX_NONE;
	UE_LOG(LogTemp, Display, TEXT("[RoutePreview] Exit -> Live visualization restored"));
	if (bRouteOverlayEnabled)
	{
		FString OverlayError;
		if (RefreshRouteOverlay(OverlayError))
		{
			UE_LOG(LogTemp, Display, TEXT("[RouteOverlay] Restored in Live mode"));
		}
	}
}

bool URoutePreviewSubsystem::SetRouteOverlayEnabled(
	const bool bEnabled,
	FString& OutError)
{
	OutError.Reset();
	bRouteOverlayEnabled = bEnabled;
	if (!bEnabled)
	{
		if (ADroneFleetVisualizationActor* VisualizationActor = FindVisualizationActor())
		{
			VisualizationActor->SetRouteTargetsVisible(false);
		}
		bRouteOverlayVisible = false;
		UE_LOG(LogTemp, Display, TEXT("[RouteOverlay] Disabled"));
		return true;
	}

	UE_LOG(LogTemp, Display, TEXT("[RouteOverlay] Enabled"));
	return RefreshRouteOverlay(OutError);
}

bool URoutePreviewSubsystem::RefreshRouteOverlay(FString& OutError)
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	OutError.Reset();
	bRouteOverlayVisible = false;
	ADroneFleetVisualizationActor* VisualizationActor = FindVisualizationActor();
	if (!VisualizationActor)
	{
		OutError = TEXT("DroneFleetVisualizationActor is unavailable in the current world.");
		UE_LOG(LogTemp, Warning, TEXT("[RouteOverlay] Requested but visualization actor is unavailable"));
		return false;
	}

	VisualizationActor->SetRouteTargetsVisible(false);
	if (!RouteData.IsLoaded())
	{
		VisualizationActor->ClearRouteTargets();
		CachedRouteTargetWorldPositions.Reset();
		OutError = TEXT("Route overlay requested but no route is loaded.");
		UE_LOG(LogTemp, Warning, TEXT("[RouteOverlay] Requested but no route is loaded"));
		return false;
	}
	if (!RouteData.bShowOriginSet)
	{
		VisualizationActor->ClearRouteTargets();
		CachedRouteTargetWorldPositions.Reset();
		OutError = TEXT("Route overlay requested but Show Origin is unavailable.");
		UE_LOG(LogTemp, Warning, TEXT("[RouteOverlay] Requested but Show Origin is unavailable"));
		return false;
	}

	if (FramePositionsENU.Num() != RouteData.DroneCount)
	{
		FramePositionsENU.SetNumUninitialized(RouteData.DroneCount);
	}
	for (int32 RouteSlotIndex = 0; RouteSlotIndex < RouteData.DroneCount; ++RouteSlotIndex)
	{
		// Frame 0 is contiguous in the frame-major route store. Array index is
		// deliberately identical to RouteSlotIndex and never a Fleet DroneID.
		FramePositionsENU[RouteSlotIndex] = RouteData.GetPosition(0, RouteSlotIndex);
	}

	FVector RouteOriginWorld = FVector::ZeroVector;
	if (!VisualizationActor->ConvertRouteENUPositionsToWorld(
		FramePositionsENU,
		RouteData.ShowOriginLatitude,
		RouteData.ShowOriginLongitude,
		RouteData.ShowOriginAltitude,
		CachedRouteTargetWorldPositions,
		RouteOriginWorld,
		OutError))
	{
		VisualizationActor->ClearRouteTargets();
		CachedRouteTargetWorldPositions.Reset();
		UE_LOG(LogTemp, Warning, TEXT("[RouteOverlay] Refresh failed: %s"), *OutError);
		return false;
	}

	if (CachedRouteTargetWorldPositions.Num() != RouteData.DroneCount)
	{
		VisualizationActor->ClearRouteTargets();
		CachedRouteTargetWorldPositions.Reset();
		OutError = TEXT("Route overlay conversion returned an unexpected target count.");
		UE_LOG(LogTemp, Warning, TEXT("[RouteOverlay] %s"), *OutError);
		return false;
	}

	VisualizationActor->SetRouteTargetPositions(CachedRouteTargetWorldPositions);
	const bool bCanShow = bRouteOverlayEnabled
		&& VisualizationActor->GetVisualizationMode() == EGCSVisualizationMode::Live;
	VisualizationActor->SetRouteTargetsVisible(bCanShow);
	bRouteOverlayVisible = bCanShow && VisualizationActor->AreRouteTargetsVisible();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RouteOverlay] Refreshed Targets=%d Frame=0 Visible=%s"),
		CachedRouteTargetWorldPositions.Num(),
		bRouteOverlayVisible ? TEXT("YES") : TEXT("NO"));
	return true;
}

void URoutePreviewSubsystem::LogRouteOverlayStatus() const
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	const ADroneFleetVisualizationActor* VisualizationActor = FindVisualizationActor();
	const EGCSVisualizationMode VisualizationMode = VisualizationActor
		? VisualizationActor->GetVisualizationMode()
		: EGCSVisualizationMode::Live;
	const TCHAR* VisualizationModeName = VisualizationActor
		? (VisualizationMode == EGCSVisualizationMode::RoutePreview
			? TEXT("RoutePreview")
			: TEXT("Live"))
		: TEXT("Unavailable");
	const int32 SubmittedTargetCount = VisualizationActor
		? VisualizationActor->GetRouteTargetCount()
		: 0;
	const bool bVisible = VisualizationActor
		? VisualizationActor->AreRouteTargetsVisible()
		: false;

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RouteOverlay] Status Enabled=%s Visible=%s RouteLoaded=%s OriginSet=%s RouteTargetCount=%d VisualizationMode=%s CachedTargetCount=%d"),
		bRouteOverlayEnabled ? TEXT("YES") : TEXT("NO"),
		bVisible ? TEXT("YES") : TEXT("NO"),
		RouteData.IsLoaded() ? TEXT("YES") : TEXT("NO"),
		RouteData.bShowOriginSet ? TEXT("YES") : TEXT("NO"),
		SubmittedTargetCount,
		VisualizationModeName,
		CachedRouteTargetWorldPositions.Num());
}

void URoutePreviewSubsystem::ClearRouteOverlayRenderData()
{
	CachedRouteTargetWorldPositions.Reset();
	bRouteOverlayVisible = false;
	if (ADroneFleetVisualizationActor* VisualizationActor = FindVisualizationActor())
	{
		VisualizationActor->ClearRouteTargets();
	}
}

bool URoutePreviewSubsystem::SetPreviewFrame(const int32 FrameIndex, FString& OutError)
{
	return SeekToFrame(FrameIndex, OutError);
}

bool URoutePreviewSubsystem::SeekToFrame(const int32 FrameIndex, FString& OutError)
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	if (PlaybackState == ERoutePreviewPlaybackState::Playing)
	{
		Pause();
	}
	else if (PlaybackState == ERoutePreviewPlaybackState::Completed)
	{
		PlaybackState = ERoutePreviewPlaybackState::Paused;
	}

	if (!ApplyPreviewFrame(FrameIndex, OutError))
	{
		return false;
	}

	if (RouteData.SampleRateHz > 0.0)
	{
		CurrentTimeSeconds = CurrentFrame / RouteData.SampleRateHz;
	}
	return true;
}

bool URoutePreviewSubsystem::SetSampleRateHz(const double Hz, FString& OutError)
{
	FRoutePreviewData& RouteData = GetMutableActiveRouteData();
	OutError.Reset();
	if (!FMath::IsFinite(Hz) || Hz <= 0.0)
	{
		OutError = TEXT("Route Preview sample rate must be finite and greater than zero.");
		return false;
	}

	RouteData.SampleRateHz = Hz;
	if (CurrentFrame >= 0)
	{
		CurrentTimeSeconds = CurrentFrame / Hz;
	}
	UE_LOG(LogTemp, Display, TEXT("[RoutePreview] SampleRate=%.6fHz Duration=%.6fs"), Hz, GetDurationSeconds());
	return true;
}

bool URoutePreviewSubsystem::Play(FString& OutError)
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	OutError.Reset();
	if (!RouteData.IsLoaded())
	{
		OutError = TEXT("No route is loaded.");
		return false;
	}
	if (!FMath::IsFinite(RouteData.SampleRateHz) || RouteData.SampleRateHz <= 0.0)
	{
		OutError = TEXT("Route Preview sample rate is not configured.");
		return false;
	}
	if (!RouteData.bShowOriginSet)
	{
		OutError = TEXT("Route preview ShowOrigin is not set.");
		return false;
	}
	if (PlaybackState == ERoutePreviewPlaybackState::Playing)
	{
		StartPlaybackTicker();
		return true;
	}

	if (PlaybackState == ERoutePreviewPlaybackState::Completed)
	{
		CurrentTimeSeconds = 0.0;
		CurrentFrame = INDEX_NONE;
	}
	const int32 StartFrame = CurrentFrame == INDEX_NONE ? 0 : CurrentFrame;
	if ((!bPreviewModeActive || CurrentFrame == INDEX_NONE)
		&& !ApplyPreviewFrame(StartFrame, OutError))
	{
		return false;
	}

	PlaybackState = ERoutePreviewPlaybackState::Playing;
	StartPlaybackTicker();
	UE_LOG(LogTemp, Display, TEXT("[RoutePreview] Play Time=%.6fs Frame=%d"), CurrentTimeSeconds, CurrentFrame);
	return true;
}

void URoutePreviewSubsystem::Pause()
{
	if (PlaybackState != ERoutePreviewPlaybackState::Playing)
	{
		return;
	}
	StopPlaybackTicker();
	PlaybackState = ERoutePreviewPlaybackState::Paused;
	UE_LOG(LogTemp, Display, TEXT("[RoutePreview] Pause Time=%.6fs Frame=%d"), CurrentTimeSeconds, CurrentFrame);
}

bool URoutePreviewSubsystem::Stop(FString& OutError)
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	OutError.Reset();
	if (!RouteData.IsLoaded())
	{
		OutError = TEXT("No route is loaded.");
		return false;
	}

	StopPlaybackTicker();
	PlaybackState = ERoutePreviewPlaybackState::Stopped;
	CurrentTimeSeconds = 0.0;
	CurrentFrame = INDEX_NONE;
	if (!ApplyPreviewFrame(0, OutError))
	{
		return false;
	}
	CurrentTimeSeconds = 0.0;
	UE_LOG(LogTemp, Display, TEXT("[RoutePreview] Stop -> Frame 0"));
	return true;
}

double URoutePreviewSubsystem::GetDurationSeconds() const
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	return RouteData.FrameCount > 0 && RouteData.SampleRateHz > 0.0
		? (RouteData.FrameCount - 1) / RouteData.SampleRateHz
		: 0.0;
}

bool URoutePreviewSubsystem::ApplyPreviewFrame(const int32 FrameIndex, FString& OutError)
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	OutError.Reset();
	if (!RouteData.IsLoaded())
	{
		OutError = TEXT("No route is loaded.");
		return false;
	}
	if (!RouteData.bShowOriginSet)
	{
		OutError = TEXT("Route preview ShowOrigin is not set.");
		return false;
	}

	const int32 ClampedFrameIndex = FMath::Clamp(FrameIndex, 0, RouteData.FrameCount - 1);
	if (bPreviewModeActive && CurrentFrame == ClampedFrameIndex)
	{
		return true;
	}

	if (!bPreviewModeActive && !EnterPreviewMode(OutError))
	{
		return false;
	}
	ADroneFleetVisualizationActor* VisualizationActor = FindVisualizationActor();
	if (!VisualizationActor)
	{
		OutError = TEXT("DroneFleetVisualizationActor is unavailable in the current world.");
		bPreviewModeActive = false;
		return false;
	}

	if (FramePositionsENU.Num() != RouteData.DroneCount)
	{
		FramePositionsENU.SetNumUninitialized(RouteData.DroneCount);
	}
	if (FrameColors.Num() != RouteData.DroneCount)
	{
		FrameColors.SetNumUninitialized(RouteData.DroneCount);
	}

	for (int32 RouteSlotIndex = 0; RouteSlotIndex < RouteData.DroneCount; ++RouteSlotIndex)
	{
		FramePositionsENU[RouteSlotIndex] =
			RouteData.GetPosition(ClampedFrameIndex, RouteSlotIndex);
		FrameColors[RouteSlotIndex] =
			RouteData.GetColor(ClampedFrameIndex, RouteSlotIndex);
	}

	if (!VisualizationActor->SetRoutePreviewFrame(
		FramePositionsENU,
		FrameColors,
		RouteData.ShowOriginLatitude,
		RouteData.ShowOriginLongitude,
		RouteData.ShowOriginAltitude,
		OutError))
	{
		return false;
	}

	CurrentFrame = ClampedFrameIndex;
	UE_LOG(
		LogTemp,
		VeryVerbose,
		TEXT("[RoutePreview] Frame requested=%d applied=%d PreviewParticles=%d"),
		FrameIndex,
		ClampedFrameIndex,
		RouteData.DroneCount);
	return true;
}

bool URoutePreviewSubsystem::TickPlayback(const float DeltaSeconds)
{
	const FRoutePreviewData& RouteData = GetActiveRouteData();
	if (PlaybackState != ERoutePreviewPlaybackState::Playing)
	{
		PlaybackTickerHandle.Reset();
		return false;
	}

	const double DurationSeconds = GetDurationSeconds();
	CurrentTimeSeconds = FMath::Min(CurrentTimeSeconds + FMath::Max(0.0f, DeltaSeconds), DurationSeconds);
	const int32 LastFrame = RouteData.FrameCount - 1;
	const int32 DesiredFrame = FMath::Clamp(
		FMath::FloorToInt(CurrentTimeSeconds * RouteData.SampleRateHz),
		0,
		LastFrame);

	if (DesiredFrame != CurrentFrame)
	{
		FString Error;
		if (!ApplyPreviewFrame(DesiredFrame, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("[RoutePreview] Playback stopped: %s"), *Error);
			PlaybackState = ERoutePreviewPlaybackState::Stopped;
			PlaybackTickerHandle.Reset();
			return false;
		}
	}

	if (CurrentTimeSeconds >= DurationSeconds)
	{
		if (CurrentFrame != LastFrame)
		{
			FString Error;
			if (!ApplyPreviewFrame(LastFrame, Error))
			{
				UE_LOG(LogTemp, Error, TEXT("[RoutePreview] Final frame failed: %s"), *Error);
				PlaybackState = ERoutePreviewPlaybackState::Stopped;
				PlaybackTickerHandle.Reset();
				return false;
			}
		}
		PlaybackState = ERoutePreviewPlaybackState::Completed;
		PlaybackTickerHandle.Reset();
		UE_LOG(LogTemp, Display, TEXT("[RoutePreview] Completed Time=%.6fs Frame=%d"), CurrentTimeSeconds, CurrentFrame);
		return false;
	}
	return true;
}

void URoutePreviewSubsystem::StartPlaybackTicker()
{
	if (PlaybackTickerHandle.IsValid())
	{
		return;
	}
	PlaybackTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &URoutePreviewSubsystem::TickPlayback));
}

void URoutePreviewSubsystem::StopPlaybackTicker()
{
	if (PlaybackTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PlaybackTickerHandle);
		PlaybackTickerHandle.Reset();
	}
}

ADroneFleetVisualizationActor* URoutePreviewSubsystem::FindVisualizationActor() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

TSharedPtr<const FRoutePreviewData, ESPMode::ThreadSafe>
URoutePreviewSubsystem::CaptureRouteDataSnapshot() const
{
	check(IsInGameThread());
	return ActiveRouteData;
}

const FRoutePreviewData& URoutePreviewSubsystem::GetActiveRouteData() const
{
	static const FRoutePreviewData Empty;
	return ActiveRouteData.IsValid() ? *ActiveRouteData : Empty;
}

FRoutePreviewData& URoutePreviewSubsystem::GetMutableActiveRouteData()
{
	check(IsInGameThread());
	if (!ActiveRouteData.IsValid())
	{
		ActiveRouteData = MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>();
	}
	return *ActiveRouteData;
}
