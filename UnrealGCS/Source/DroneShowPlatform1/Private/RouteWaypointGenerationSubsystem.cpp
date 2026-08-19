#include "RouteWaypointGenerationSubsystem.h"

#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "RouteArtifactWorkspaceSubsystem.h"
#include "RoutePreviewSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogRouteWaypointGeneration, Log, All);

namespace
{
	int64 UtcNowUnixMilliseconds()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}
}

void URouteWaypointGenerationSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<URouteArtifactWorkspaceSubsystem>();
}

void URouteWaypointGenerationSubsystem::Deinitialize()
{
	bDeinitializing = true;
	StopCompletionTicker();
	if (ActiveBatch)
	{
		ActiveBatch->Cancel();
		ActiveBatch->Wait();
		LastProgress = ActiveBatch->GetProgress();
	}
	if (FinalizeFuture.IsValid())
	{
		FinalizeFuture.Wait();
		FinalizeFuture.Get();
	}
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (URouteArtifactWorkspaceSubsystem* Workspace =
			GameInstance->GetSubsystem<URouteArtifactWorkspaceSubsystem>())
		{
			if (Workspace->GetGenerationStatus() != ERouteArtifactGenerationStatus::None)
			{
				FString AbortError;
				Workspace->AbortGeneration(AbortError);
			}
		}
	}
	ActiveBatch.Reset();
	PendingManifestDraft = FRouteArtifactManifestV2{};
	PendingSourceValidationContext = FRouteWaypointSourceValidationContext{};
	bFinalizeStarted = false;
	Super::Deinitialize();
}

bool URouteWaypointGenerationSubsystem::StartGeneration(
	const int32 WorkerCountOverride,
	FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (bDeinitializing || IsGenerationActive())
	{
		OutError = TEXT("Route waypoint generation is already active or shutting down.");
		return false;
	}
	ActiveBatch.Reset();
	LastProgress = FRouteWaypointBatchProgress{};
	LastGeneratedManifest = FRouteArtifactManifestV2{};
	bFinalizeStarted = false;

	FRouteWaypointBatchRequest Request;
	FRouteArtifactManifestV2 Draft;
	if (!BuildStartInputs(WorkerCountOverride, Request, Draft, OutError))
	{
		return false;
	}
	ActiveBatch = MakeUnique<FRouteWaypointBatchGenerator>();
	if (!ActiveBatch->Start(Request, OutError))
	{
		if (URouteArtifactWorkspaceSubsystem* Workspace = GetGameInstance()
			->GetSubsystem<URouteArtifactWorkspaceSubsystem>())
		{
			FString AbortError;
			Workspace->AbortGeneration(AbortError);
		}
		ActiveBatch.Reset();
		return false;
	}
	PendingManifestDraft = MoveTemp(Draft);
	PendingSourceValidationContext = Request.Snapshot.MakeSourceValidationContext();
	CompletionTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(
			this, &URouteWaypointGenerationSubsystem::TickGeneration), 0.05f);
	return true;
}

bool URouteWaypointGenerationSubsystem::CancelGeneration(FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (!ActiveBatch
		|| (ActiveBatch->IsFinished() && !bFinalizeStarted))
	{
		OutError = TEXT("No active route waypoint generation can be cancelled.");
		return false;
	}
	ActiveBatch->Cancel();
	return true;
}

bool URouteWaypointGenerationSubsystem::IsGenerationActive() const
{
	// Remains active through the GameThread Finalize/Abort continuation.
	return ActiveBatch != nullptr;
}

FRouteWaypointBatchProgress URouteWaypointGenerationSubsystem::GetProgress() const
{
	return ActiveBatch ? ActiveBatch->GetProgress() : LastProgress;
}

bool URouteWaypointGenerationSubsystem::TickGeneration(const float DeltaSeconds)
{
	if (bDeinitializing || !ActiveBatch)
	{
		CompletionTickerHandle.Reset();
		return false;
	}
	if (!ActiveBatch->IsFinished())
	{
		return true;
	}

	// Futures are already complete; this releases their handles without a GameThread wait loop.
	ActiveBatch->Wait();
	FRouteWaypointBatchProgress WorkerProgress = ActiveBatch->GetProgress();
	URouteArtifactWorkspaceSubsystem* Workspace = GetGameInstance()
		? GetGameInstance()->GetSubsystem<URouteArtifactWorkspaceSubsystem>() : nullptr;
	if (WorkerProgress.GenerationState == ERouteWaypointBatchState::WorkersSucceeded
		&& Workspace && !bFinalizeStarted)
	{
		FRouteArtifactWorkspaceSourceGate FinalizeGate;
		FString GateError;
		FRouteArtifactWorkspaceManager* Manager = Workspace->GetGenerationManager();
		if (!Manager || !Workspace->CaptureSourceGate(FinalizeGate, GateError))
		{
			if (Manager && Manager->HasActiveGeneration())
			{
				FString AbortError;
				Manager->AbortGeneration(AbortError);
			}
			ActiveBatch->MarkFailed(
				ERouteWaypointBatchErrorCode::AuthorityChanged, GateError);
		}
		else
		{
			ActiveBatch->MarkFinalizing();
			FRouteArtifactManifestV2 Draft = MoveTemp(PendingManifestDraft);
			FRouteWaypointSourceValidationContext SourceValidationContext =
				MoveTemp(PendingSourceValidationContext);
			FRouteWaypointBatchGenerator* const Batch = ActiveBatch.Get();
			FinalizeFuture = Async(EAsyncExecution::ThreadPool,
				[Manager, Gate = MoveTemp(FinalizeGate), Draft = MoveTemp(Draft),
					SourceValidationContext = MoveTemp(SourceValidationContext), Batch]() mutable
				{
					FRouteWaypointFinalizeAsyncResult Result;
					Result.Manifest = MoveTemp(Draft);
					Result.bSuccess = Manager->FinalizeGeneratedRoutes(
						Result.Manifest, Gate, SourceValidationContext, Result.Error, nullptr,
						[Batch]() { return Batch->IsCancellationRequested(); });
					return Result;
				});
		}
		bFinalizeStarted = true;
		return true;
	}
	if (bFinalizeStarted && FinalizeFuture.IsValid())
	{
		if (!FinalizeFuture.IsReady())
		{
			return true;
		}
		FRouteWaypointFinalizeAsyncResult Result = FinalizeFuture.Get();
		if (Result.bSuccess)
		{
			LastGeneratedManifest = MoveTemp(Result.Manifest);
			ActiveBatch->MarkSucceeded();
		}
		else
		{
			if (ActiveBatch->IsCancellationRequested()
				|| Result.Error.Contains(TEXT("Cancelled"), ESearchCase::IgnoreCase))
			{
				ActiveBatch->MarkCancelled(Result.Error);
			}
			else
			{
				const ERouteWaypointBatchErrorCode Code =
					Result.Error.Contains(TEXT("Gate"))
					|| Result.Error.Contains(TEXT("source"), ESearchCase::IgnoreCase)
					? ERouteWaypointBatchErrorCode::AuthorityChanged
					: ERouteWaypointBatchErrorCode::FinalizeFailed;
				ActiveBatch->MarkFailed(Code, Result.Error);
			}
		}
	}
	else if (!bFinalizeStarted)
	{
		if (Workspace
			&& Workspace->GetGenerationStatus() != ERouteArtifactGenerationStatus::None)
		{
			FString AbortError;
			Workspace->AbortGeneration(AbortError);
		}
	}

	LastProgress = ActiveBatch->GetProgress();
	ActiveBatch.Reset();
	PendingManifestDraft = FRouteArtifactManifestV2{};
	PendingSourceValidationContext = FRouteWaypointSourceValidationContext{};
	bFinalizeStarted = false;
	CompletionTickerHandle.Reset();
	return false;
}

bool URouteWaypointGenerationSubsystem::BuildStartInputs(
	const int32 WorkerCountOverride,
	FRouteWaypointBatchRequest& OutRequest,
	FRouteArtifactManifestV2& OutManifestDraft,
	FString& OutError)
{
	OutRequest = FRouteWaypointBatchRequest{};
	OutManifestDraft = FRouteArtifactManifestV2{};
	UGameInstance* GameInstance = GetGameInstance();
	URoutePreviewSubsystem* RoutePreview = GameInstance
		? GameInstance->GetSubsystem<URoutePreviewSubsystem>() : nullptr;
	URouteArtifactWorkspaceSubsystem* Workspace = GameInstance
		? GameInstance->GetSubsystem<URouteArtifactWorkspaceSubsystem>() : nullptr;
	if (!RoutePreview || !Workspace)
	{
		OutError = TEXT("Route generation source or Workspace subsystem is unavailable.");
		return false;
	}

	const TSharedPtr<const FRoutePreviewData, ESPMode::ThreadSafe> RouteData =
		RoutePreview->CaptureRouteDataSnapshot();
	if (!RouteData.IsValid() || !RouteData->IsLoaded() || !RouteData->bShowOriginSet)
	{
		OutError = TEXT("A loaded Route dataset with Show Origin is required.");
		return false;
	}
	FRouteWaypointGeneratorContext GeneratorContext;
	FRouteWaypointGeneratorError GeneratorError;
	if (!RouteWaypointGenerator::BuildContext(
		RouteData->ShowOriginLatitude,
		RouteData->ShowOriginLongitude,
		GeneratorContext,
		GeneratorError))
	{
		OutError = GeneratorError.Diagnostic;
		return false;
	}

	FRouteArtifactBuildIdentityV2 BuildIdentity;
	BuildIdentity.SourceDatasetDigest = RouteData->SourceIdentity.SourceDatasetDigest;
	BuildIdentity.WaypointGeneratorVersion = GeneratorContext.WaypointGeneratorVersion;
	BuildIdentity.GeneratorInputDigest = GeneratorContext.GeneratorInputDigest;
	BuildIdentity.ShowOriginLatitudeE7 = GeneratorContext.NormalizedShowOriginLatitudeE7;
	BuildIdentity.ShowOriginLongitudeE7 = GeneratorContext.NormalizedShowOriginLongitudeE7;
	BuildIdentity.FrameCount = RouteData->FrameCount;
	BuildIdentity.RouteSlotCount = RouteData->DroneCount;
	BuildIdentity.bHasRange = RouteData->OptionalRangeStart != INDEX_NONE
		&& RouteData->OptionalRangeEnd != INDEX_NONE;
	BuildIdentity.RangeStart = BuildIdentity.bHasRange
		? RouteData->OptionalRangeStart : 0;
	BuildIdentity.RangeEnd = BuildIdentity.bHasRange
		? RouteData->OptionalRangeEnd : 0;

	FRouteArtifactWorkspaceSourceGate SourceGate;
	SourceGate.bSourceExists = true;
	SourceGate.ActiveSourceIdentity = RouteData->SourceIdentity;
	SourceGate.RouteDatasetGeneration = RoutePreview->GetRouteDatasetGeneration();
	SourceGate.FrameCount = BuildIdentity.FrameCount;
	SourceGate.RouteSlotCount = BuildIdentity.RouteSlotCount;
	SourceGate.bHasRange = BuildIdentity.bHasRange;
	SourceGate.RangeStart = BuildIdentity.RangeStart;
	SourceGate.RangeEnd = BuildIdentity.RangeEnd;
	SourceGate.ShowOriginLatitudeE7 = BuildIdentity.ShowOriginLatitudeE7;
	SourceGate.ShowOriginLongitudeE7 = BuildIdentity.ShowOriginLongitudeE7;
	SourceGate.WaypointGeneratorVersion = BuildIdentity.WaypointGeneratorVersion;
	SourceGate.GeneratorInputDigest = BuildIdentity.GeneratorInputDigest;

	int64 FreeBytes = 0;
	int64 RequiredWithMargin = 0;
	if (!Workspace->GetWorkspaceVolumeFreeBytes(FreeBytes, OutError)
		|| !FRouteWaypointBatchGenerator::CheckDiskCapacity(
			RouteData->FrameCount, RouteData->DroneCount,
			FreeBytes, RequiredWithMargin, OutError))
	{
		return false;
	}

	FRouteArtifactGenerationHandle Handle;
	if (!Workspace->BeginGeneration(BuildIdentity, SourceGate, Handle, OutError))
	{
		return false;
	}

	OutRequest.Snapshot.RouteData = RouteData;
	OutRequest.Snapshot.SourceIdentity = RouteData->SourceIdentity;
	OutRequest.Snapshot.RouteDatasetGeneration = SourceGate.RouteDatasetGeneration;
	OutRequest.Snapshot.GeneratorContext = GeneratorContext;
	OutRequest.Snapshot.WaypointGeneratorVersion = GeneratorContext.WaypointGeneratorVersion;
	OutRequest.Snapshot.GeneratorInputDigest = GeneratorContext.GeneratorInputDigest;
	OutRequest.Snapshot.FrameCount = RouteData->FrameCount;
	OutRequest.Snapshot.RouteCount = RouteData->DroneCount;
	OutRequest.WorkspaceManager = Workspace->GetGenerationManager();
	OutRequest.AttemptID = Handle.AttemptID;
	OutRequest.ArtifactSetID = Handle.ArtifactSetID;
	OutRequest.WorkerCountOverride = WorkerCountOverride;

	OutManifestDraft.ArtifactSetID = Handle.ArtifactSetID;
	OutManifestDraft.CreatedAtUtc = UtcNowUnixMilliseconds();
	OutManifestDraft.SourceIdentity = RouteData->SourceIdentity;
	OutManifestDraft.FrameCount = RouteData->FrameCount;
	OutManifestDraft.RouteSlotCount = RouteData->DroneCount;
	OutManifestDraft.bHasRange = BuildIdentity.bHasRange;
	OutManifestDraft.RangeStart = BuildIdentity.RangeStart;
	OutManifestDraft.RangeEnd = BuildIdentity.RangeEnd;
	OutManifestDraft.ShowOriginLatitudeE7 = BuildIdentity.ShowOriginLatitudeE7;
	OutManifestDraft.ShowOriginLongitudeE7 = BuildIdentity.ShowOriginLongitudeE7;
	OutManifestDraft.WaypointGeneratorVersion = GeneratorContext.WaypointGeneratorVersion;
	OutManifestDraft.GeneratorInputDigest = GeneratorContext.GeneratorInputDigest;
	return true;
}

void URouteWaypointGenerationSubsystem::StopCompletionTicker()
{
	if (CompletionTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(CompletionTickerHandle);
		CompletionTickerHandle.Reset();
	}
}
