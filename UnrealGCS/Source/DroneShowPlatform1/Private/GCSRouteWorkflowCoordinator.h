#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/Object.h"

#include "GCSRouteWorkflowCoordinator.generated.h"

class UButton;
class UGameInstance;
class UUserWidget;
class UWidget;

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

#if WITH_DEV_AUTOMATION_TESTS
	void InitializeForTest(UGameInstance* InGameInstance);
	void SetWorkflowFactoryForTest(
		TFunction<UUserWidget*()> InFactory,
		bool bInSkipRequiredWidgetBinding);
	UUserWidget* GetWorkflowWidgetForTest() const { return RouteWorkflowWidget; }
	int32 GetWorkflowCreationCountForTest() const { return WorkflowCreationCount; }
	bool IsRefreshTickerActiveForTest() const { return RefreshTickerHandle.IsValid(); }
	bool IsBindingValidForTest() const { return bBindingValid; }
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
	FTSTicker::FDelegateHandle RefreshTickerHandle;
	bool bBindingValid = false;

#if WITH_DEV_AUTOMATION_TESTS
	TFunction<UUserWidget*()> WorkflowFactoryForTest;
	bool bSkipRequiredWidgetBindingForTest = false;
	int32 WorkflowCreationCount = 0;
#endif

	static constexpr float RefreshIntervalSeconds = 0.2f;
};
