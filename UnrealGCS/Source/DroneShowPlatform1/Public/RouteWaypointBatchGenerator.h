#pragma once

#include "CoreMinimal.h"
#include "RouteWaypointGenerator.h"
#include "RouteWaypointValidator.h"

class FRouteArtifactWorkspaceManager;

enum class ERouteWaypointBatchState : uint8
{
	Idle,
	Generating,
	Cancelling,
	WorkersSucceeded,
	Finalizing,
	Succeeded,
	Failed,
	Cancelled
};

enum class ERouteWaypointBatchErrorCode : uint8
{
	None,
	InvalidSnapshot,
	InvalidWorkerCount,
	InsufficientDiskSpace,
	WorkspaceBeginFailed,
	RouteFailed,
	AuthorityChanged,
	FinalizeFailed,
	Cancelled,
	InternalError
};

/** GameThread-captured immutable generation authority and shared dataset lifetime. */
struct DRONESHOWPLATFORM1_API FRouteWaypointGenerationSnapshot
{
	TSharedPtr<const FRoutePreviewData, ESPMode::ThreadSafe> RouteData;
	FRouteArtifactSourceIdentity SourceIdentity;
	int64 RouteDatasetGeneration = 0;
	FRouteWaypointGeneratorContext GeneratorContext;
	FString WaypointGeneratorVersion;
	FRouteArtifactDigest256 GeneratorInputDigest;
	int32 FrameCount = 0;
	int32 RouteCount = 0;

	bool IsValid() const;
	FRouteWaypointSourceValidationContext MakeSourceValidationContext() const;
};

struct DRONESHOWPLATFORM1_API FRouteWaypointBatchProgress
{
	int32 TotalRoutes = 0;
	int32 QueuedRoutes = 0;
	int32 ActiveRoutes = 0;
	int32 CompletedRoutes = 0;
	int32 FailedRoutes = 0;
	int64 TotalRowsExpected = 0;
	int64 RowsWritten = 0;
	int64 BytesWritten = 0;
	int64 ClampedSampleCount = 0;
	int64 ClampedChannelCount = 0;
	FString AttemptID;
	FString ArtifactSetID;
	int32 WorkerCount = 0;
	double ElapsedSeconds = 0.0;
	ERouteWaypointBatchState GenerationState = ERouteWaypointBatchState::Idle;
	ERouteWaypointBatchErrorCode LastErrorCode = ERouteWaypointBatchErrorCode::None;
	ERouteWaypointGeneratorErrorCode FirstFailureErrorCode =
		ERouteWaypointGeneratorErrorCode::None;
	int32 FirstFailureRouteSlotIndex = INDEX_NONE;
	FString FirstFailureDiagnostic;
	int32 PeakActiveWorkers = 0;
	int32 ActiveRouteFiles = 0;
	int32 PeakOpenRouteFiles = 0;
	int32 RouteFailures = 0;
	int32 CancellationCount = 0;
};

struct DRONESHOWPLATFORM1_API FRouteWaypointBatchRequest
{
	FRouteWaypointGenerationSnapshot Snapshot;
	FRouteArtifactWorkspaceManager* WorkspaceManager = nullptr;
	FString AttemptID;
	FString ArtifactSetID;
	/** Zero selects the runtime default. */
	int32 WorkerCountOverride = 0;

#if WITH_DEV_AUTOMATION_TESTS
	int32 TestFailWriterRouteSlot = INDEX_NONE;
	int32 TestDelayPerRouteMilliseconds = 0;
#endif
};

/**
 * Bounded long-lived worker coordinator. It owns exactly WorkerCount tasks;
 * workers read only the immutable snapshot and the pure Workspace manager.
 */
class DRONESHOWPLATFORM1_API FRouteWaypointBatchGenerator final
{
public:
	FRouteWaypointBatchGenerator();
	~FRouteWaypointBatchGenerator();

	FRouteWaypointBatchGenerator(const FRouteWaypointBatchGenerator&) = delete;
	FRouteWaypointBatchGenerator& operator=(const FRouteWaypointBatchGenerator&) = delete;

	bool Start(const FRouteWaypointBatchRequest& Request, FString& OutError);
	void Cancel();
	bool IsCancellationRequested() const;
	bool IsFinished() const;
	void Wait();
	FRouteWaypointBatchProgress GetProgress() const;
	TArray<FRouteWaypointGenerationResult> GetRouteResultsCopy() const;

	void MarkFinalizing();
	void MarkSucceeded();
	void MarkCancelled(const FString& SafeDiagnostic);
	void MarkFailed(ERouteWaypointBatchErrorCode ErrorCode, const FString& SafeDiagnostic);

	static int32 GetDefaultWorkerCount(int32 RouteCount);
	static int32 GetProfiledDefaultWorkerCap();
	static int32 GetMaximumWorkerOverride(int32 RouteCount);
	static int64 GetMaxWaypointRowBytesV1();
	static bool ComputeMaxRouteFileBytesV1(
		int32 FrameCount,
		int64& OutBytes,
		FString& OutError);
	static bool ComputeOutputUpperBound(
		int32 FrameCount,
		int32 RouteCount,
		int64& OutBytes,
		FString& OutError);
	static bool CheckDiskCapacity(
		int32 FrameCount,
		int32 RouteCount,
		int64 AvailableBytes,
		int64& OutRequiredWithMargin,
		FString& OutError);

private:
	struct FImpl;
	TSharedPtr<FImpl, ESPMode::ThreadSafe> Impl;
};
