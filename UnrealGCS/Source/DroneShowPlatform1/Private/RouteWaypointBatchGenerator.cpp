#include "RouteWaypointBatchGenerator.h"

#include "Async/Async.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "RouteArtifactWorkspaceManager.h"

namespace
{
	constexpr int64 MinimumDiskSafetyMarginBytes = 64ll * 1024 * 1024;
	// C2B filesystem calibration on the production 1000x9100 data shape found
	// eight workers to be the stable throughput/ground-station resource balance.
	constexpr int32 ProfiledDefaultWorkerCap = 8;
	// These cover canonical manifest records and filesystem allocation metadata.
	// They are operational estimates, separate from the strict v1 file bound.
	constexpr int64 ManifestFixedEstimateBytes = 64ll * 1024;
	constexpr int64 ManifestPerRouteEstimateBytes = 512;
	constexpr int64 FilesystemOverheadPerRouteBytes = 4096;

	bool SameSource(
		const FRouteArtifactSourceIdentity& Left,
		const FRouteArtifactSourceIdentity& Right)
	{
		return Left.IsValid() && Right.IsValid()
			&& Left.SourceDatasetDigest == Right.SourceDatasetDigest
			&& Left.SourceByteSize == Right.SourceByteSize
			&& Left.SourceDisplayFileName == Right.SourceDisplayFileName;
	}
}

bool FRouteWaypointGenerationSnapshot::IsValid() const
{
	return RouteData.IsValid()
		&& RouteData->IsLoaded()
		&& SameSource(SourceIdentity, RouteData->SourceIdentity)
		&& RouteDatasetGeneration > 0
		&& GeneratorContext.IsValid()
		&& WaypointGeneratorVersion == RouteWaypointGeneratorContract::WaypointGeneratorVersion
		&& WaypointGeneratorVersion == GeneratorContext.WaypointGeneratorVersion
		&& GeneratorInputDigest == GeneratorContext.GeneratorInputDigest
		&& FrameCount == RouteData->FrameCount
		&& RouteCount == RouteData->DroneCount
		&& FrameCount > 0
		&& RouteCount > 0;
}

FRouteWaypointSourceValidationContext
FRouteWaypointGenerationSnapshot::MakeSourceValidationContext() const
{
	FRouteWaypointSourceValidationContext Context;
	Context.RouteData = RouteData;
	Context.SourceIdentity = SourceIdentity;
	Context.RouteDatasetGeneration = RouteDatasetGeneration;
	Context.GeneratorContext = GeneratorContext;
	Context.FrameCount = FrameCount;
	Context.RouteCount = RouteCount;
	return Context;
}

struct FRouteWaypointBatchGenerator::FImpl
{
	mutable FCriticalSection Mutex;
	FCriticalSection ClaimMutex;
	FRouteWaypointBatchRequest Request;
	FRouteWaypointBatchProgress Progress;
	TArray<FRouteWaypointGenerationResult> RouteResults;
	TArray<TFuture<void>> Workers;
	FRouteWaypointFileActivityTracker FileActivity;
	TAtomic<bool> bCancellationRequested{false};
	TAtomic<int32> RemainingWorkers{0};
	int32 NextRouteSlot = 0;
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
	TAtomic<bool> bUserCancellation{false};
	bool bFirstFailureRecorded = false;

	bool ClaimRoute(int32& OutRouteSlot)
	{
		FScopeLock ClaimLock(&ClaimMutex);
		if (bCancellationRequested.Load()
			|| NextRouteSlot >= Request.Snapshot.RouteCount)
		{
			return false;
		}
		OutRouteSlot = NextRouteSlot++;
		return true;
	}

	void RequestCancellation(const bool bFromUser)
	{
		FScopeLock ClaimLock(&ClaimMutex);
		bCancellationRequested.Store(true);
		if (bFromUser)
		{
			bUserCancellation.Store(true);
		}
		if (bFromUser)
		{
			FScopeLock Lock(&Mutex);
			if (Progress.GenerationState == ERouteWaypointBatchState::Generating
				|| Progress.GenerationState == ERouteWaypointBatchState::Finalizing)
			{
				Progress.GenerationState = ERouteWaypointBatchState::Cancelling;
				Progress.LastErrorCode = ERouteWaypointBatchErrorCode::Cancelled;
			}
		}
	}

	void RecordRouteStart()
	{
		FScopeLock Lock(&Mutex);
		++Progress.ActiveRoutes;
		Progress.PeakActiveWorkers = FMath::Max(
			Progress.PeakActiveWorkers, Progress.ActiveRoutes);
	}

	void RecordRouteResult(
		const int32 RouteSlotIndex,
		const FRouteWaypointGenerationResult& Result)
	{
		bool bMustCancel = false;
		{
			FScopeLock Lock(&Mutex);
			RouteResults[RouteSlotIndex] = Result;
			--Progress.ActiveRoutes;
			Progress.RowsWritten += Result.RowCount;
			Progress.BytesWritten += Result.BytesWritten;
			Progress.ClampedSampleCount += Result.ClampedSampleCount;
			Progress.ClampedChannelCount += Result.ClampedChannelCount;
			if (Result.bSuccess)
			{
				++Progress.CompletedRoutes;
			}
			else if (Result.ErrorCode == ERouteWaypointGeneratorErrorCode::Cancelled)
			{
				++Progress.CancellationCount;
			}
			else
			{
				++Progress.FailedRoutes;
				++Progress.RouteFailures;
				if (!bFirstFailureRecorded)
				{
					bFirstFailureRecorded = true;
					Progress.FirstFailureRouteSlotIndex = RouteSlotIndex;
					Progress.FirstFailureErrorCode = Result.ErrorCode;
					Progress.FirstFailureDiagnostic = Result.Diagnostic.Left(256);
					Progress.LastErrorCode = ERouteWaypointBatchErrorCode::RouteFailed;
				}
				bMustCancel = true;
			}
		}
		if (bMustCancel)
		{
			RequestCancellation(false);
		}
	}

	void WorkerMain()
	{
		for (;;)
		{
			int32 RouteSlotIndex = INDEX_NONE;
			if (!ClaimRoute(RouteSlotIndex))
			{
				break;
			}
			RecordRouteStart();

#if WITH_DEV_AUTOMATION_TESTS
			if (Request.TestDelayPerRouteMilliseconds > 0)
			{
				FPlatformProcess::SleepNoStats(
					Request.TestDelayPerRouteMilliseconds / 1000.0f);
			}
#endif

			FRouteWaypointGenerationResult Result;
#if WITH_DEV_AUTOMATION_TESTS
			if (RouteSlotIndex == Request.TestFailWriterRouteSlot)
			{
				Result.RouteSlotIndex = RouteSlotIndex;
				Result.ErrorCode = ERouteWaypointGeneratorErrorCode::FileWriteFailed;
				Result.Diagnostic = TEXT("Injected route writer failure.");
			}
			else
#endif
			{
				FRouteWaypointGeneratorInput Input;
				Input.RouteData = Request.Snapshot.RouteData.Get();
				Input.RouteSlotIndex = RouteSlotIndex;
				Input.SourceIdentity = Request.Snapshot.SourceIdentity;
				Input.Context = Request.Snapshot.GeneratorContext;
				Result = RouteWaypointGenerator::GenerateRouteToStagingFile(
					*Request.WorkspaceManager,
					Input,
					[this]() { return bCancellationRequested.Load(); },
					&FileActivity);
			}
			RecordRouteResult(RouteSlotIndex, Result);
		}

		if (--RemainingWorkers == 0)
		{
			FScopeLock Lock(&Mutex);
			EndSeconds = FPlatformTime::Seconds();
			if (bFirstFailureRecorded)
			{
				Progress.GenerationState = ERouteWaypointBatchState::Failed;
			}
			else if (bUserCancellation.Load())
			{
				Progress.GenerationState = ERouteWaypointBatchState::Cancelled;
				Progress.LastErrorCode = ERouteWaypointBatchErrorCode::Cancelled;
			}
			else
			{
				Progress.GenerationState = ERouteWaypointBatchState::WorkersSucceeded;
			}
		}
	}
};

FRouteWaypointBatchGenerator::FRouteWaypointBatchGenerator()
	: Impl(MakeShared<FImpl, ESPMode::ThreadSafe>())
{
}

FRouteWaypointBatchGenerator::~FRouteWaypointBatchGenerator()
{
	Cancel();
	Wait();
}

bool FRouteWaypointBatchGenerator::Start(
	const FRouteWaypointBatchRequest& Request,
	FString& OutError)
{
	OutError.Reset();
	if (!Request.Snapshot.IsValid() || Request.WorkspaceManager == nullptr)
	{
		OutError = TEXT("Batch generation snapshot or Workspace manager is invalid.");
		return false;
	}
	const int32 MaximumWorkers = GetMaximumWorkerOverride(Request.Snapshot.RouteCount);
	const int32 WorkerCount = Request.WorkerCountOverride == 0
		? GetDefaultWorkerCount(Request.Snapshot.RouteCount)
		: Request.WorkerCountOverride;
	if (WorkerCount < 1 || WorkerCount > MaximumWorkers)
	{
		OutError = FString::Printf(
			TEXT("Worker override must be in [1,%d] for this runtime route domain."),
			MaximumWorkers);
		return false;
	}

	{
		FScopeLock Lock(&Impl->Mutex);
		if (Impl->Progress.GenerationState != ERouteWaypointBatchState::Idle)
		{
			OutError = TEXT("Batch generator has already been started.");
			return false;
		}
		Impl->Request = Request;
		Impl->Progress.TotalRoutes = Request.Snapshot.RouteCount;
		Impl->Progress.QueuedRoutes = Request.Snapshot.RouteCount;
		Impl->Progress.TotalRowsExpected =
			static_cast<int64>(Request.Snapshot.FrameCount) * Request.Snapshot.RouteCount;
		Impl->Progress.AttemptID = Request.AttemptID;
		Impl->Progress.ArtifactSetID = Request.ArtifactSetID;
		Impl->Progress.WorkerCount = WorkerCount;
		Impl->Progress.GenerationState = ERouteWaypointBatchState::Generating;
		Impl->RouteResults.SetNum(Request.Snapshot.RouteCount);
		Impl->StartSeconds = FPlatformTime::Seconds();
		Impl->RemainingWorkers.Store(WorkerCount);
		Impl->Workers.Reserve(WorkerCount);
	}

	for (int32 WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
	{
		TSharedPtr<FImpl, ESPMode::ThreadSafe> SharedImpl = Impl;
		Impl->Workers.Add(Async(EAsyncExecution::ThreadPool,
			[SharedImpl]() { SharedImpl->WorkerMain(); }));
	}
	return true;
}

void FRouteWaypointBatchGenerator::Cancel()
{
	if (Impl)
	{
		Impl->RequestCancellation(true);
	}
}

bool FRouteWaypointBatchGenerator::IsCancellationRequested() const
{
	return Impl && Impl->bCancellationRequested.Load();
}

bool FRouteWaypointBatchGenerator::IsFinished() const
{
	return !Impl || Impl->RemainingWorkers.Load() == 0;
}

void FRouteWaypointBatchGenerator::Wait()
{
	if (!Impl)
	{
		return;
	}
	for (TFuture<void>& Worker : Impl->Workers)
	{
		Worker.Wait();
	}
}

FRouteWaypointBatchProgress FRouteWaypointBatchGenerator::GetProgress() const
{
	FRouteWaypointBatchProgress Result;
	if (!Impl)
	{
		return Result;
	}
	int32 NextRouteSlot = 0;
	{
		FScopeLock ClaimLock(&Impl->ClaimMutex);
		NextRouteSlot = Impl->NextRouteSlot;
	}
	FScopeLock Lock(&Impl->Mutex);
	Result = Impl->Progress;
	Result.QueuedRoutes = FMath::Max(0,
		Impl->Request.Snapshot.RouteCount - NextRouteSlot);
	Result.ActiveRouteFiles = Impl->FileActivity.GetActiveRouteFileCount();
	Result.PeakOpenRouteFiles = Impl->FileActivity.GetPeakActiveRouteFileCount();
	const double End = Impl->EndSeconds > 0.0
		? Impl->EndSeconds : FPlatformTime::Seconds();
	Result.ElapsedSeconds = Impl->StartSeconds > 0.0
		? End - Impl->StartSeconds : 0.0;
	return Result;
}

TArray<FRouteWaypointGenerationResult>
FRouteWaypointBatchGenerator::GetRouteResultsCopy() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->RouteResults;
}

void FRouteWaypointBatchGenerator::MarkFinalizing()
{
	FScopeLock Lock(&Impl->Mutex);
	Impl->Progress.GenerationState = ERouteWaypointBatchState::Finalizing;
}

void FRouteWaypointBatchGenerator::MarkSucceeded()
{
	FScopeLock Lock(&Impl->Mutex);
	Impl->Progress.GenerationState = ERouteWaypointBatchState::Succeeded;
	Impl->Progress.LastErrorCode = ERouteWaypointBatchErrorCode::None;
}

void FRouteWaypointBatchGenerator::MarkCancelled(const FString& SafeDiagnostic)
{
	FScopeLock Lock(&Impl->Mutex);
	Impl->Progress.GenerationState = ERouteWaypointBatchState::Cancelled;
	Impl->Progress.LastErrorCode = ERouteWaypointBatchErrorCode::Cancelled;
	if (!Impl->bFirstFailureRecorded)
	{
		Impl->Progress.FirstFailureDiagnostic = SafeDiagnostic.Left(256);
	}
}

void FRouteWaypointBatchGenerator::MarkFailed(
	const ERouteWaypointBatchErrorCode ErrorCode,
	const FString& SafeDiagnostic)
{
	FScopeLock Lock(&Impl->Mutex);
	Impl->Progress.GenerationState = ERouteWaypointBatchState::Failed;
	Impl->Progress.LastErrorCode = ErrorCode;
	if (!Impl->bFirstFailureRecorded)
	{
		Impl->Progress.FirstFailureDiagnostic = SafeDiagnostic.Left(256);
	}
}

int32 FRouteWaypointBatchGenerator::GetDefaultWorkerCount(const int32 RouteCount)
{
	return RouteCount > 0
		? FMath::Min3(RouteCount, FMath::Max(1, FPlatformMisc::NumberOfCores()),
			ProfiledDefaultWorkerCap)
		: 0;
}

int32 FRouteWaypointBatchGenerator::GetProfiledDefaultWorkerCap()
{
	return ProfiledDefaultWorkerCap;
}

int32 FRouteWaypointBatchGenerator::GetMaximumWorkerOverride(const int32 RouteCount)
{
	const int32 RuntimeSafetyBound = FMath::Max(1, FPlatformMisc::NumberOfCores() * 2);
	return RouteCount > 0 ? FMath::Min(RouteCount, RuntimeSafetyBound) : 0;
}

int64 FRouteWaypointBatchGenerator::GetMaxWaypointRowBytesV1()
{
	return RouteWaypointGeneratorContract::MaxWaypointRowBytesV1;
}

bool FRouteWaypointBatchGenerator::ComputeMaxRouteFileBytesV1(
	const int32 FrameCount,
	int64& OutBytes,
	FString& OutError)
{
	OutBytes = 0;
	OutError.Reset();
	if (FrameCount <= 0)
	{
		OutError = TEXT("FrameCount must be positive for disk preflight.");
		return false;
	}
	constexpr int64 RowBytes =
		RouteWaypointGeneratorContract::MaxWaypointRowBytesIncludingCrLfV1;
	if (FrameCount
		> (MAX_int64 - RouteWaypointGeneratorContract::HeaderBytes) / RowBytes)
	{
		OutError = TEXT("Per-route waypoint output upper bound overflowed int64.");
		return false;
	}
	OutBytes = RouteWaypointGeneratorContract::HeaderBytes
		+ static_cast<int64>(FrameCount) * RowBytes;
	return true;
}

bool FRouteWaypointBatchGenerator::ComputeOutputUpperBound(
	const int32 FrameCount,
	const int32 RouteCount,
	int64& OutBytes,
	FString& OutError)
{
	OutBytes = 0;
	OutError.Reset();
	if (RouteCount <= 0)
	{
		OutError = TEXT("RouteCount must be positive for disk preflight.");
		return false;
	}
	int64 PerRoute = 0;
	if (!ComputeMaxRouteFileBytesV1(FrameCount, PerRoute, OutError))
	{
		return false;
	}
	if (PerRoute > MAX_int64 / RouteCount)
	{
		OutError = TEXT("Artifact waypoint output upper bound overflowed int64.");
		return false;
	}
	const int64 RouteBytes = PerRoute * RouteCount;
	const int64 PerRouteMetadata =
		ManifestPerRouteEstimateBytes + FilesystemOverheadPerRouteBytes;
	if (RouteCount > (MAX_int64 - ManifestFixedEstimateBytes) / PerRouteMetadata)
	{
		OutError = TEXT("Artifact metadata upper estimate overflowed int64.");
		return false;
	}
	const int64 MetadataBytes = ManifestFixedEstimateBytes
		+ static_cast<int64>(RouteCount) * PerRouteMetadata;
	if (RouteBytes > MAX_int64 - MetadataBytes)
	{
		OutError = TEXT("Artifact output upper estimate overflowed int64.");
		return false;
	}
	OutBytes = RouteBytes + MetadataBytes;
	return true;
}

bool FRouteWaypointBatchGenerator::CheckDiskCapacity(
	const int32 FrameCount,
	const int32 RouteCount,
	const int64 AvailableBytes,
	int64& OutRequiredWithMargin,
	FString& OutError)
{
	OutRequiredWithMargin = 0;
	int64 UpperBound = 0;
	if (AvailableBytes < 0
		|| !ComputeOutputUpperBound(FrameCount, RouteCount, UpperBound, OutError))
	{
		if (OutError.IsEmpty()) OutError = TEXT("Available disk capacity is invalid.");
		return false;
	}
	const int64 PercentageMargin = UpperBound / 10;
	const int64 Margin = FMath::Max(MinimumDiskSafetyMarginBytes, PercentageMargin);
	if (UpperBound > MAX_int64 - Margin)
	{
		OutError = TEXT("Waypoint output plus safety margin overflowed int64.");
		return false;
	}
	OutRequiredWithMargin = UpperBound + Margin;
	if (AvailableBytes < OutRequiredWithMargin)
	{
		OutError = TEXT("InsufficientDiskSpace: Workspace volume free space is below the conservative generation requirement.");
		return false;
	}
	return true;
}
