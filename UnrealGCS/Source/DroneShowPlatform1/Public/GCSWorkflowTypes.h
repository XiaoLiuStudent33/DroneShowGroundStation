#pragma once
#include "CoreMinimal.h"

// Existing workflow contracts moved out of the private Widget coordinator.
enum class EGCSRouteAutoStartState : uint8
{
	Idle,
	ValidatingMapping,
	Freezing,
	SyncingConsole,
	StartingJob,
	// The local orchestration dispatched Start; this is not Console acceptance.
	Completed,
	Failed,
	CancelledAsStale
};

struct FGCSConfirmedNpzMappingEntry
{
	FString DroneID;
	FString EndpointIdentity;
	int32 RouteSlotIndex = INDEX_NONE;
	double MatchDistanceMeters = 0.0;
};

/** Session-only operator confirmation. Upload authority remains Working/Frozen Assignment. */
struct FGCSConfirmedNpzMappingSnapshot
{
	int64 ConfirmationRevision = 0;
	int64 RouteDatasetGeneration = 0;
	FString SourceDatasetDigest;
	int32 RouteCount = 0;
	int32 NormalizedShowOriginLatitudeE7 = 0;
	int32 NormalizedShowOriginLongitudeE7 = 0;
	FString F1CaptureAuthoritySessionID;
	FString F1CaptureRequestID;
	int64 F1CaptureRevision = 0;
	FString SelectionDigest;
	TArray<FString> SelectedDroneIDs;
	FString FleetSessionID;
	TArray<FGCSConfirmedNpzMappingEntry> Entries;
	/** Selected participants without an executable one-to-one RouteSlot match. */
	TArray<FString> UnassignedDroneIDs;
	/** Subset of UnassignedDroneIDs classified as excess/spare by the matcher. */
	TArray<FString> SpareDroneIDs;
	/** RouteSlots left vacant by this degraded confirmation. */
	TArray<int32> VacantRouteSlots;
	bool bValid = false;
	FString InvalidReason;
};

struct FGCSRouteWorkflowCommandGate
{
	bool bRouteBrowseEnabled = false;
	bool bOutputBrowseEnabled = false;
	bool bLoadPreviewEnabled = false;
	bool bGenerateWaypointsEnabled = false;
};

struct FGCSRouteUploadCommandGate
{
	bool bStartEnabled = false;
	bool bPauseEnabled = false;
	bool bResumeEnabled = false;
	bool bCancelEnabled = false;
	bool bRetryFailedEnabled = false;
	bool bApplyMaxEnabled = false;
	FString StartBlockedReason;
};


enum class EGCSWorkflowAction : uint8
{
    CaptureStableGps, ConfirmMapping, ClearMapping, GenerateWaypoints,
    StartUpload, PauseUpload, ResumeUpload, CancelUpload, RetryFailed
};

struct FGCSUploadRouteView
{
    int32 RouteSlotIndex = INDEX_NONE;
    FString DroneID;
    FString StateText;
    FString Diagnostic;
    bool bAuthoritative = false;
    uint64 BytesCompleted = 0;
    uint64 TotalBytes = 0;
    int32 RetryCount = 0;
};

// Read-only UI projection, not a second transaction or upload mirror.
struct FGCSWorkflowView
{
    FGCSRouteWorkflowCommandGate RouteActions;
    FGCSRouteUploadCommandGate UploadActions;
    FGCSConfirmedNpzMappingSnapshot ConfirmedMapping;
    bool bHasConfirmedMapping = false;
    bool bSelectionLocked = false;
    EGCSRouteAutoStartState PreparationState = EGCSRouteAutoStartState::Idle;
    FString SelectedRoutePath;
    FString Diagnostic;
    FString UploadJobID;
    FString UploadStatusText;
    int32 AppliedConcurrency = 50;
    bool bGenerating = false, bArtifactReady = false;
    int32 GeneratedRoutes = 0, TotalGenerationRoutes = 0;
    FString GenerationDiagnostic, ArtifactSetID;
    bool bHasJobSnapshot = false;
    bool bRoutesComplete = false;
    uint64 BytesCompleted = 0, TotalBytes = 0, ElapsedMs = 0, ThroughputBytesPerSecond = 0;
    uint32 Matched = 0, Vacant = 0, Spare = 0, Waiting = 0, Preparing = 0;
    uint32 Uploading = 0, RetryWaiting = 0, Succeeded = 0, Failed = 0, Cancelled = 0;
    TArray<FGCSUploadRouteView> Routes;
};

struct FGCSMissionView
{
    TArray<FString> TargetDroneIDs;
    FString PendingCommandID;
    FDateTime ScheduledLocalTime;
    bool bCountdownActive = false;
    int32 TargetCount = 0, ResultCount = 0, SucceededCount = 0;
};
