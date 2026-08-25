#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "RouteAssignmentTypes.h"
#include "UObject/Object.h"

#include "GCSRouteWorkflowCoordinator.generated.h"

class UButton;
class UGameInstance;
class UListView;
class UUserWidget;
class UWidget;

enum class EGCSRouteAutoStartState : uint8
{
	Idle,
	ValidatingMapping,
	Freezing,
	SyncingConsole,
	StartingJob,
	Completed,
	Failed,
	CancelledAsStale
};

struct FGCSRouteAutoStartTargetSnapshot
{
	FString DroneID;
	FString EndpointIdentity;
	int32 NumericDroneID = 0;
	int32 RouteSlotIndex = INDEX_NONE;
	FString RouteRelativePath;
	int64 RouteFileByteSize = 0;
	FString RouteDigest;
};

struct FGCSRouteAutoStartSnapshot
{
	FString TransactionID;
	TArray<FString> SelectedDroneIDs;
	FString FleetSessionID;
	int64 FleetRevision = 0;
	FString ArtifactSetID;
	FString ArtifactSetDigest;
	FString SourceDatasetDigest;
	int32 RouteSlotCount = 0;
	int32 MaxConcurrentTargets = 1;
	TArray<FGCSRouteAutoStartTargetSnapshot> Targets;
	FRouteAssignmentSourceIdentity AssignmentSourceIdentity;
	FString AssignmentAuthoritySessionID;
	int64 AssignmentRevision = 0;
	FString AssignmentDigest;
	FString SyncTransactionID;
	bool bJobStartAttempted = false;
	bool bJobStartDispatched = false;
};

struct FGCSRouteAutoStartSyncObservation
{
	uint8 State = 0;
	FString FleetSessionID;
	FString AssignmentAuthoritySessionID;
	int64 AssignmentRevision = 0;
	FString AssignmentDigest;
	FString TransactionID;
	FString LastError;
};

/** Stable, identity-only ListView item. Route state remains in the Phase 2G mirror. */
UCLASS()
class UGCSRouteUploadListItem final : public UObject
{
	GENERATED_BODY()

public:
	int32 GetRouteSlotIndex() const { return RouteSlotIndex; }
	const FString& GetUploadJobID() const { return UploadJobID; }
	const FString& GetSnapshotToken() const { return SnapshotToken; }
	bool IsAutoStartPreview() const { return bAutoStartPreview; }
	const FString& GetPreviewDroneID() const { return PreviewDroneID; }
	const FString& GetPreviewRouteRelativePath() const { return PreviewRouteRelativePath; }

private:
	friend class UGCSRouteWorkflowCoordinator;
	int32 RouteSlotIndex = INDEX_NONE;
	FString UploadJobID;
	FString SnapshotToken;
	bool bAutoStartPreview = false;
	FString PreviewDroneID;
	FString PreviewRouteRelativePath;
};

struct FGCSRouteUploadArtifactDisplay
{
	FString ArtifactSetID;
	FString WaypointsRoot;
	FString SourceDataset;
	bool bArtifactMismatch = false;
};

struct FGCSRouteWorkflowCommandGateInputs
{
	bool bHasPreparedRoute = false;
	bool bOriginValid = false;
	bool bRouteLoaded = false;
	bool bShowOriginSet = false;
	bool bWorkspaceReady = false;
	bool bGenerationActive = false;
	bool bCommandInFlight = false;
};

struct FGCSRouteWorkflowCommandGate
{
	bool bRouteBrowseEnabled = false;
	bool bOutputBrowseEnabled = false;
	bool bLoadPreviewEnabled = false;
	bool bGenerateWaypointsEnabled = false;
};

enum class EGCSRouteWorkflowCommandErrorCategory : uint8
{
	None,
	Dataset,
	Preview,
	Artifact,
	Upload
};

struct FGCSRouteUploadCommandGateInputs
{
	bool bArtifactReady = false;
	bool bFrozenConsoleAccepted = false;
	bool bAutoStartMappingValid = false;
	bool bAutoStartInFlight = false;
	FString StartBlockedReason;
	bool bHasActiveJobBinding = false;
	bool bHasExecutableJob = false;
	bool bCommandInFlight = false;
	uint8 JobState = 0;
	uint32 FailedCount = 0;
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

#if WITH_DEV_AUTOMATION_TESTS
struct FGCSRouteWorkflowCommandTestHooks
{
	TFunction<bool(FString&)> OpenNpzDialog;
	TFunction<bool(FString&)> OpenFolderDialog;
	TFunction<bool(const FString&, FString&)> PrepareRoute;
	TFunction<bool(FString&)> CommitRoute;
	TFunction<bool(double, double, double, bool, FString&)> CommitRouteWorkflow;
	TFunction<bool(double, double, double, FString&)> SetOrigin;
	TFunction<bool(bool, FString&)> SetOverlay;
	TFunction<bool(FString&)> CanReplaceUploadJob;
	TFunction<bool(FString&)> ClearUploadJobForNewDataset;
	TFunction<bool(const FString&, FString&)> SetWorkspaceRoot;
	TFunction<bool(FString&, FString&)> ResolveWorkspaceRoot;
	TFunction<bool(int32, FString&)> StartGeneration;
};

struct FGCSRouteAutoStartTestHooks
{
	TFunction<bool(FGCSRouteAutoStartSnapshot&, FString&)> CaptureSnapshot;
	TFunction<bool(FGCSRouteAutoStartSnapshot&, FString&)> BuildAssignment;
	TFunction<bool(FGCSRouteAutoStartSnapshot&, FString&)> FreezeAssignment;
	TFunction<bool(FGCSRouteAutoStartSnapshot&, FString&)> StartSync;
	TFunction<FGCSRouteAutoStartSyncObservation()> ObserveSync;
	TFunction<bool(const FGCSRouteAutoStartSnapshot&, FString&)> ValidateSnapshot;
	TFunction<bool(int32, FString&, FString&)> StartJob;
};
#endif

/**
 * Presentation-only owner for the Route Workflow window.
 *
 * All displayed values remain owned by their production subsystems. This
 * coordinator owns one widget instance, validates its authored hierarchy, and
 * copies authoritative read-only snapshots into UMG on the Game Thread.
 */
UCLASS()
class UGCSRouteWorkflowCoordinator final : public UObject
{
	GENERATED_BODY()

public:
	bool Initialize(UGameInstance* InGameInstance, UUserWidget* InRightPanelWidget);
	void Shutdown();
	bool ShowRouteWorkflow(FString& OutError);
	bool IsCommandSelectionMutationBlocked() const;

	static double ComputeProgressRatio(uint64 Completed, uint64 Total);
	static FString FormatByteCount(uint64 Bytes);
	static FString FormatLogicalByteProgress(uint64 Completed, uint64 Total);
	static FString FormatThroughput(uint64 BytesPerSecond);
	static FString FormatElapsedMilliseconds(uint64 ElapsedMilliseconds);
	static FString FormatUploadJobPresentation(uint8 JobStateValue);
	static FString FormatPercent(double Ratio);
	static int32 ComputeRetryCount(uint32 AttemptCount);
	static FString FormatRouteState(uint8 RouteStateValue);
	static FString FormatRouteErrorCode(uint16 ErrorCodeValue);
	static FString DiagnoseRouteRowStructure(const UUserWidget* Row);
	static FString FormatRouteFileDisplay(const FString& ManifestRelativePath);
	static FGCSRouteUploadArtifactDisplay ResolveArtifactDisplay(
		bool bHasJob,
		const FString& JobArtifactSetID,
		const FString& JobArtifactDigest,
		bool bCurrentUsable,
		const FString& CurrentArtifactSetID,
		const FString& CurrentArtifactDigest,
		const FString& CurrentRoot,
		const FString& CurrentSourceDataset);
	static bool ParseOriginInputs(
		const FString& LatitudeText,
		const FString& LongitudeText,
		const FString& AltitudeText,
		double& OutLatitude,
		double& OutLongitude,
		double& OutAltitude,
		FString& OutError);
	static FGCSRouteWorkflowCommandGate ComputeCommandGate(
		const FGCSRouteWorkflowCommandGateInputs& Inputs);
	static bool ParsePositiveConcurrency(
		const FString& Text,
		int32& OutValue,
		FString& OutError);
	static FGCSRouteUploadCommandGate ComputeUploadCommandGate(
		const FGCSRouteUploadCommandGateInputs& Inputs);
	static FString FormatAutoStartState(EGCSRouteAutoStartState State);

#if WITH_DEV_AUTOMATION_TESTS
	void InitializeForTest(UGameInstance* InGameInstance);
	void SetWorkflowFactoryForTest(
		TFunction<UUserWidget*()> InFactory,
		bool bInSkipRequiredWidgetBinding);
	UUserWidget* GetWorkflowWidgetForTest() const { return RouteWorkflowWidget; }
	int32 GetWorkflowCreationCountForTest() const { return WorkflowCreationCount; }
	bool IsRefreshTickerActiveForTest() const { return RefreshTickerHandle.IsValid(); }
	bool IsBindingValidForTest() const { return bBindingValid; }
	bool BuildRouteItemsForTest(
		const FString& JobID,
		const FString& SnapshotToken,
		const TArray<int32>& SortedRouteSlots);
	int32 GetRouteItemCountForTest() const { return RouteListItems.Num(); }
	int64 GetRouteListRebuildCountForTest() const { return RouteListRebuildCount; }
	const UGCSRouteUploadListItem* FindRouteItemForTest(int32 RouteSlotIndex) const;
	static uint64 EstimateRouteItemModelBytes(int32 ItemCount);
	void RefreshVisibleRouteRowsForTest() { RefreshVisibleRouteRows(); }
	void SetCommandTestHooks(FGCSRouteWorkflowCommandTestHooks InHooks)
	{
		CommandTestHooks = MoveTemp(InHooks);
	}
	void SetCommandGateInputsForTest(const FGCSRouteWorkflowCommandGateInputs& Inputs)
	{
		CommandGateInputsForTest = Inputs;
	}
	void ClearCommandGateInputsForTest() { CommandGateInputsForTest.Reset(); }
	void ClickRouteBrowseForTest() { HandleRouteBrowseClicked(); }
	void ClickOutputBrowseForTest() { HandleOutputBrowseClicked(); }
	void ClickLoadPreviewForTest() { HandleLoadPreviewClicked(); }
	void ClickGenerateWaypointsForTest() { HandleGenerateWaypointsClicked(); }
	void ClickCloseForTest() { HandleRouteCloseClicked(); }
	void ClickConvertTabForTest() { HandleRouteConvertTabClicked(); }
	void ClickUploadTabForTest() { HandleRouteUploadTabClicked(); }
	void ClickApplyMaxConcurrencyForTest()
	{
		HandleApplyMaxConcurrencyClicked();
	}
	void RefreshCommandControlsForTest() { UpdateCommandControlState(); }
	void RefreshReadOnlyStateForTest() { RefreshReadOnlyState(); }
	const FString& GetSelectedRoutePathForTest() const { return SelectedRouteImportFilePath; }
	const FString& GetCommandErrorForTest() const { return CommandErrorMessage; }
	double GetCommandErrorExpiryForTest() const { return CommandErrorExpiresAtSeconds; }
	bool IsViewVisibleForTest() const { return bViewVisible; }
	int32 GetSelectedPageIndexForTest() const { return SelectedPageIndex; }
	int32 GetCloseRequestCountForTest() const { return CloseRequestCountForTest; }
	void SetAutoStartTestHooks(FGCSRouteAutoStartTestHooks InHooks)
	{
		AutoStartTestHooks = MoveTemp(InHooks);
	}
	bool StartAutoStartForTest(FString& OutError) { return BeginAutoStartTransaction(OutError); }
	void AdvanceAutoStartForTest() { AdvanceAutoStartTransaction(); }
	bool TickRefreshForTest() { return TickRefresh(0.0f); }
	EGCSRouteAutoStartState GetAutoStartStateForTest() const { return AutoStartState; }
	const FGCSRouteAutoStartSnapshot& GetAutoStartSnapshotForTest() const
	{
		return AutoStartSnapshot;
	}
	int32 GetAppliedMaxConcurrentTargetsForTest() const
	{
		return AppliedMaxConcurrentTargets;
	}
	void SetAppliedMaxConcurrentTargetsForTest(const int32 Value)
	{
		AppliedMaxConcurrentTargets = Value;
	}
#endif

private:
	UFUNCTION()
	void HandleRouteWorkflowClicked();
	UFUNCTION()
	void HandleRouteCloseClicked();
	UFUNCTION()
	void HandleRouteConvertTabClicked();
	UFUNCTION()
	void HandleRouteUploadTabClicked();
	UFUNCTION()
	void HandleRouteBrowseClicked();
	UFUNCTION()
	void HandleOutputBrowseClicked();
	UFUNCTION()
	void HandleLoadPreviewClicked();
	UFUNCTION()
	void HandleGenerateWaypointsClicked();
	UFUNCTION()
	void HandleStartUploadClicked();
	UFUNCTION()
	void HandlePauseUploadClicked();
	UFUNCTION()
	void HandleResumeUploadClicked();
	UFUNCTION()
	void HandleCancelUploadClicked();
	UFUNCTION()
	void HandleRetryFailedClicked();
	UFUNCTION()
	void HandleApplyMaxConcurrencyClicked();

	UUserWidget* CreateRouteWorkflowWidget(FString& OutError);
	bool BindWorkflowWidgets(UUserWidget* WorkflowWidget, FString& OutError);
	bool ResolveRequiredWidgets(
		UUserWidget* OwnerWidget,
		const TArray<TPair<FName, UClass*>>& Requirements,
		TMap<FName, TWeakObjectPtr<UWidget>>& OutWidgets,
		FString& OutError) const;
	UUserWidget* ResolveNestedPage(
		UUserWidget* WorkflowWidget,
		FName PreferredInstanceName,
		FName UniqueRequiredChildName) const;
	void ApplyReadOnlyControlState();
	void SelectRoutePage(int32 PageIndex);
	void BindCommandControls();
	void UnbindCommandControls();
	FGCSRouteWorkflowCommandGateInputs GatherCommandGateInputs() const;
	void UpdateCommandControlState();
	FGCSRouteUploadCommandGateInputs GatherUploadCommandGateInputs() const;
	void UpdateUploadCommandControlState();
	bool ReadConcurrencyDraft(int32& OutValue, FString& OutError) const;
	void SetCommandError(EGCSRouteWorkflowCommandErrorCategory Category, const FString& Message);
	void ClearCommandError(EGCSRouteWorkflowCommandErrorCategory Category);
	void ApplyCommandErrorPresentation();
	void RefreshReadOnlyState();
	void RefreshArtifactIdentity();
	void SyncRouteListModel();
	void RefreshVisibleRouteRows();
	bool ReplaceRouteListItems(
		const FString& JobID,
		const FString& SnapshotToken,
		const TArray<int32>& SortedRouteSlots);
	void ClearRouteListModel();
	bool ReplaceAutoStartPreviewItems(const FGCSRouteAutoStartSnapshot& Snapshot);
	bool CaptureAutoStartSnapshot(FGCSRouteAutoStartSnapshot& OutSnapshot, FString& OutError) const;
	bool BeginAutoStartTransaction(FString& OutError);
	void AdvanceAutoStartTransaction();
	bool ValidateAutoStartSnapshotStillCurrent(FString& OutError) const;
	FGCSRouteAutoStartSyncObservation ObserveAutoStartSync() const;
	void FinishAutoStart(EGCSRouteAutoStartState FinalState, const FString& Diagnostic);
	void ResetAutoStartForNewDataset();
	bool IsAutoStartInFlight() const;
	bool TickRefresh(float DeltaSeconds);
	void StartRefreshTicker();
	void StopRefreshTicker();

	TWeakObjectPtr<UGameInstance> GameInstanceRef;
	TWeakObjectPtr<UUserWidget> RightPanelWidgetRef;
	TWeakObjectPtr<UButton> OpenWorkflowButtonRef;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> RouteWorkflowWidget;

	TWeakObjectPtr<UUserWidget> RouteConvertWidgetRef;
	TWeakObjectPtr<UUserWidget> RouteUploadWidgetRef;
	TMap<FName, TWeakObjectPtr<UWidget>> WorkflowWidgets;
	TMap<FName, TWeakObjectPtr<UWidget>> ConvertWidgets;
	TMap<FName, TWeakObjectPtr<UWidget>> UploadWidgets;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UGCSRouteUploadListItem>> RouteListItems;

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UGCSRouteUploadListItem>> RouteItemsBySlot;

	FString RouteListJobID;
	FString RouteListSnapshotToken;
	int64 RouteListRebuildCount = 0;
	TSet<TWeakObjectPtr<UUserWidget>> ValidatedRouteRows;
	TMap<TWeakObjectPtr<UUserWidget>, FString> RouteRowDiagnostics;
	bool bUploadArtifactMismatch = false;
	FString SelectedRouteImportFilePath;
	FString SelectedRouteSourceDigest;
	FString CommandErrorMessage;
	double CommandErrorExpiresAtSeconds = 0.0;
	EGCSRouteWorkflowCommandErrorCategory CommandErrorCategory =
		EGCSRouteWorkflowCommandErrorCategory::None;
	bool bCommandInFlight = false;
	FTSTicker::FDelegateHandle RefreshTickerHandle;
	bool bBindingValid = false;
	bool bViewVisible = false;
	int32 SelectedPageIndex = 0;
	int32 AppliedMaxConcurrentTargets = 1;
	EGCSRouteAutoStartState AutoStartState = EGCSRouteAutoStartState::Idle;
	FGCSRouteAutoStartSnapshot AutoStartSnapshot;
	FString AutoStartDiagnostic;

#if WITH_DEV_AUTOMATION_TESTS
	TFunction<UUserWidget*()> WorkflowFactoryForTest;
	bool bSkipRequiredWidgetBindingForTest = false;
	int32 WorkflowCreationCount = 0;
	int32 CloseRequestCountForTest = 0;
	FGCSRouteWorkflowCommandTestHooks CommandTestHooks;
	FGCSRouteAutoStartTestHooks AutoStartTestHooks;
	TOptional<FGCSRouteWorkflowCommandGateInputs> CommandGateInputsForTest;
#endif

	static constexpr float RefreshIntervalSeconds = 0.2f;
	static constexpr double CommandErrorLifetimeSeconds = 5.0;
};
