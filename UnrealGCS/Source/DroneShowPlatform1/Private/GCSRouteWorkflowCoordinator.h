#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/Object.h"

#include "GCSRouteWorkflowCoordinator.generated.h"

class UButton;
class UGameInstance;
class UListView;
class UUserWidget;
class UWidget;

/** Stable, identity-only ListView item. Route state remains in the Phase 2G mirror. */
UCLASS()
class UGCSRouteUploadListItem final : public UObject
{
	GENERATED_BODY()

public:
	int32 GetRouteSlotIndex() const { return RouteSlotIndex; }
	const FString& GetUploadJobID() const { return UploadJobID; }
	const FString& GetSnapshotToken() const { return SnapshotToken; }

private:
	friend class UGCSRouteWorkflowCoordinator;
	int32 RouteSlotIndex = INDEX_NONE;
	FString UploadJobID;
	FString SnapshotToken;
};

struct FGCSRouteUploadArtifactDisplay
{
	FString ArtifactSetID;
	FString WaypointsRoot;
	FString SourceDataset;
	bool bArtifactMismatch = false;
};

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

	static double ComputeProgressRatio(uint64 Completed, uint64 Total);
	static FString FormatByteCount(uint64 Bytes);
	static FString FormatThroughput(uint64 BytesPerSecond);
	static FString FormatElapsedMilliseconds(uint64 ElapsedMilliseconds);
	static FString FormatPercent(double Ratio);
	static int32 ComputeRetryCount(uint32 AttemptCount);
	static FString FormatRouteState(uint8 RouteStateValue);
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
#endif

private:
	UFUNCTION()
	void HandleRouteWorkflowClicked();

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
	void RefreshReadOnlyState();
	void RefreshArtifactIdentity();
	void SyncRouteListModel();
	void RefreshVisibleRouteRows();
	bool ReplaceRouteListItems(
		const FString& JobID,
		const FString& SnapshotToken,
		const TArray<int32>& SortedRouteSlots);
	void ClearRouteListModel();
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
	bool bUploadArtifactMismatch = false;
	FTSTicker::FDelegateHandle RefreshTickerHandle;
	bool bBindingValid = false;

#if WITH_DEV_AUTOMATION_TESTS
	TFunction<UUserWidget*()> WorkflowFactoryForTest;
	bool bSkipRequiredWidgetBindingForTest = false;
	int32 WorkflowCreationCount = 0;
#endif

	static constexpr float RefreshIntervalSeconds = 0.2f;
};
