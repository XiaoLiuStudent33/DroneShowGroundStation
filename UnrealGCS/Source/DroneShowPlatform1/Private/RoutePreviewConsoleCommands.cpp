#include "RoutePreviewSubsystem.h"

#include "DroneFleetVisualizationActor.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING
namespace DroneGCSRoutePreviewConsolePrivate
{
	URoutePreviewSubsystem* GetSubsystem(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<URoutePreviewSubsystem>() : nullptr;
	}

	void LogResult(const bool bSuccess, const FString& Error)
	{
		if (!bSuccess)
		{
			UE_LOG(LogTemp, Error, TEXT("[RoutePreview] %s"), *Error);
		}
	}

	void Load(const TArray<FString>& Args, UWorld* World)
	{
		URoutePreviewSubsystem* Subsystem = GetSubsystem(World);
		if (!Subsystem || Args.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: RoutePreview.Load <absolute-npz-path>"));
			return;
		}
		FString Error;
		LogResult(Subsystem->LoadRoute(FString::Join(Args, TEXT(" ")), Error), Error);
	}

	void Origin(const TArray<FString>& Args, UWorld* World)
	{
		URoutePreviewSubsystem* Subsystem = GetSubsystem(World);
		double Latitude = 0.0;
		double Longitude = 0.0;
		double Altitude = 0.0;
		if (!Subsystem || Args.Num() != 3
			|| !LexTryParseString(Latitude, *Args[0])
			|| !LexTryParseString(Longitude, *Args[1])
			|| !LexTryParseString(Altitude, *Args[2]))
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: RoutePreview.Origin <latitude> <longitude> <altitude-m>"));
			return;
		}
		FString Error;
		LogResult(Subsystem->SetShowOrigin(Latitude, Longitude, Altitude, Error), Error);
	}

	void Frame(const TArray<FString>& Args, UWorld* World)
	{
		URoutePreviewSubsystem* Subsystem = GetSubsystem(World);
		int32 FrameIndex = 0;
		if (!Subsystem || Args.Num() != 1 || !LexTryParseString(FrameIndex, *Args[0]))
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: RoutePreview.Frame <frame-index>"));
			return;
		}
		FString Error;
		LogResult(Subsystem->SetPreviewFrame(FrameIndex, Error), Error);
	}

	void Rate(const TArray<FString>& Args, UWorld* World)
	{
		URoutePreviewSubsystem* Subsystem = GetSubsystem(World);
		double Hz = 0.0;
		if (!Subsystem || Args.Num() != 1 || !LexTryParseString(Hz, *Args[0]))
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: RoutePreview.Rate <Hz>"));
			return;
		}
		FString Error;
		LogResult(Subsystem->SetSampleRateHz(Hz, Error), Error);
	}

	void Play(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			FString Error;
			LogResult(Subsystem->Play(Error), Error);
		}
	}

	void Pause(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			Subsystem->Pause();
		}
	}

	void Stop(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			FString Error;
			LogResult(Subsystem->Stop(Error), Error);
		}
	}

	const TCHAR* PlaybackStateName(const ERoutePreviewPlaybackState State)
	{
		switch (State)
		{
		case ERoutePreviewPlaybackState::Stopped: return TEXT("Stopped");
		case ERoutePreviewPlaybackState::Playing: return TEXT("Playing");
		case ERoutePreviewPlaybackState::Paused: return TEXT("Paused");
		case ERoutePreviewPlaybackState::Completed: return TEXT("Completed");
		default: return TEXT("Unknown");
		}
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[RoutePreview] Status Loaded=%s Preview=%s State=%s Frames=%d Drones=%d SampleRate=%.6fHz Time=%.6fs Frame=%d Duration=%.6fs"),
				Subsystem->IsRouteLoaded() ? TEXT("YES") : TEXT("NO"),
				Subsystem->IsPreviewModeActive() ? TEXT("YES") : TEXT("NO"),
				PlaybackStateName(Subsystem->GetPlaybackState()),
				Subsystem->GetFrameCount(),
				Subsystem->GetDroneCount(),
				Subsystem->GetSampleRateHz(),
				Subsystem->GetCurrentTime(),
				Subsystem->GetCurrentFrame(),
				Subsystem->GetDurationSeconds());
		}
	}

	void Exit(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			Subsystem->ExitPreviewMode();
		}
	}

	void Unload(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			Subsystem->UnloadRoute();
		}
	}

	void OverlayOn(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			FString Error;
			Subsystem->SetRouteOverlayEnabled(true, Error);
		}
	}

	void OverlayOff(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			FString Error;
			Subsystem->SetRouteOverlayEnabled(false, Error);
		}
	}

	void OverlayRefresh(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			FString Error;
			Subsystem->RefreshRouteOverlay(Error);
		}
	}

	void OverlayStatus(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (URoutePreviewSubsystem* Subsystem = GetSubsystem(World))
		{
			Subsystem->LogRouteOverlayStatus();
		}
	}

	void FlightMarkerStatus(const TArray<FString>& Args, UWorld* World)
	{
		(void)Args;
		if (World)
		{
			for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
			{
				It->LogFlightMarkerStatus();
				return;
			}
		}
		UE_LOG(LogTemp, Warning, TEXT("[FlightMarker] DroneFleetVisualizationActor is unavailable."));
	}

	FAutoConsoleCommandWithWorldAndArgs LoadCommand(
		TEXT("RoutePreview.Load"),
		TEXT("Load a legacy route NPZ into memory."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Load));
	FAutoConsoleCommandWithWorldAndArgs OriginCommand(
		TEXT("RoutePreview.Origin"),
		TEXT("Set route preview geographic origin: latitude longitude altitude-m."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Origin));
	FAutoConsoleCommandWithWorldAndArgs FrameCommand(
		TEXT("RoutePreview.Frame"),
		TEXT("Enter preview mode and render one route frame."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Frame));
	FAutoConsoleCommandWithWorldAndArgs RateCommand(
		TEXT("RoutePreview.Rate"),
		TEXT("Set the authoritative route preview sample rate in Hz."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Rate));
	FAutoConsoleCommandWithWorldAndArgs PlayCommand(
		TEXT("RoutePreview.Play"),
		TEXT("Play or resume the loaded route preview."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Play));
	FAutoConsoleCommandWithWorldAndArgs PauseCommand(
		TEXT("RoutePreview.Pause"),
		TEXT("Pause route preview playback at the current time."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Pause));
	FAutoConsoleCommandWithWorldAndArgs StopCommand(
		TEXT("RoutePreview.Stop"),
		TEXT("Stop route preview playback and display frame zero."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Stop));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(
		TEXT("RoutePreview.Status"),
		TEXT("Print route preview load, playback, time, and frame status."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs ExitCommand(
		TEXT("RoutePreview.Exit"),
		TEXT("Exit preview mode and restore the latest live FleetManager snapshot."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Exit));
	FAutoConsoleCommandWithWorldAndArgs UnloadCommand(
		TEXT("RoutePreview.Unload"),
		TEXT("Exit preview mode and unload route memory."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Unload));
	FAutoConsoleCommandWithWorldAndArgs OverlayOnCommand(
		TEXT("RouteOverlay.On"),
		TEXT("Enable and refresh the Live + Route Frame-0 target overlay."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&OverlayOn));
	FAutoConsoleCommandWithWorldAndArgs OverlayOffCommand(
		TEXT("RouteOverlay.Off"),
		TEXT("Hide the route target overlay while retaining its preference cache."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&OverlayOff));
	FAutoConsoleCommandWithWorldAndArgs OverlayRefreshCommand(
		TEXT("RouteOverlay.Refresh"),
		TEXT("Rebuild route target world positions from route frame zero."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&OverlayRefresh));
	FAutoConsoleCommandWithWorldAndArgs OverlayStatusCommand(
		TEXT("RouteOverlay.Status"),
		TEXT("Print route overlay authority, visibility, mode, and target counts."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&OverlayStatus));
	FAutoConsoleCommandWithWorldAndArgs FlightMarkerStatusCommand(
		TEXT("GCS.FlightMarker.Status"),
		TEXT("Print the shared Flight Marker size, projection, viewport, and visibility state."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FlightMarkerStatus));
}
#endif
