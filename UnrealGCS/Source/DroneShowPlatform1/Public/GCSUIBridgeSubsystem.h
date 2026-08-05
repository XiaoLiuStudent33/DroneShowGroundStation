#pragma once

#include "CoreMinimal.h"
#include "DSPGCSProductionCommandTypes.h"
#include "DroneData.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GCSUIBridgeSubsystem.generated.h"

class UButton;
class UTextBlock;
class UUserWidget;

enum class EGCSMainPage : uint8
{
	Fleet,
	Control,
	Map,
	Settings,
	Logs
};

UCLASS()
class DRONESHOWPLATFORM1_API UGCSUIBridgeSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	void BindMainWidget(UUserWidget* MainWidget);
	virtual void Deinitialize() override;

private:
	UFUNCTION()
	void HandleMenuClicked();

	UFUNCTION()
	void HandleFleetPageClicked();

	UFUNCTION()
	void HandleControlPageClicked();

	UFUNCTION()
	void HandleMapPageClicked();

	UFUNCTION()
	void HandleSettingsPageClicked();

	UFUNCTION()
	void HandleLogsPageClicked();

	UFUNCTION()
	void HandleArmClicked();

	UFUNCTION()
	void HandleDisarmClicked();

	UFUNCTION()
	void HandleGuidedClicked();

	UFUNCTION()
	void HandleTakeoffClicked();

	UFUNCTION()
	void HandleRTLClicked();

	UFUNCTION()
	void HandleLandClicked();

	UFUNCTION()
	void HandleHoverClicked();

	UFUNCTION()
	void HandleStopClicked();

	UFUNCTION()
	void HandleFleetRow1SelectionClicked();

	UFUNCTION()
	void HandleFleetRow2SelectionClicked();

	UFUNCTION()
	void HandleFleetRow3SelectionClicked();

	UFUNCTION()
	void HandleFleetRow4SelectionClicked();

	UFUNCTION()
	void HandleFleetRow5SelectionClicked();

	UFUNCTION()
	void HandleFleetPreviousPageClicked();

	UFUNCTION()
	void HandleFleetNextPageClicked();

	UFUNCTION()
	void HandleHoverAltitudeMinusClicked();

	UFUNCTION()
	void HandleHoverAltitudePlusClicked();

	UFUNCTION()
	void HandleManualModeClicked();

	UFUNCTION()
	void HandleAutonomousModeClicked();

	UFUNCTION()
	void HandleProductionCommandResult(
		const FGCSProductionCommandResult& Result);

	UFUNCTION()
	void HandleFleetChanged();

	UFUNCTION()
	void HandleDroneDataUpdated(const FDroneData& DroneData);

	UFUNCTION()
	void HandleFleetDataBatchApplied(int64 FleetRevision, int32 UpdatedDroneCount);

	UUserWidget* ResolveDroneDetail(UUserWidget* MainWidget) const;
	UUserWidget* ResolveChildWidget(UUserWidget* MainWidget, FName Name) const;
	UButton* FindButton(const FName Name) const;
	UButton* FindFleetButton(const FName Name) const;
	UButton* FindToolbarButton(const FName Name) const;
	UTextBlock* FindText(const FName Name) const;
	UTextBlock* FindFleetText(const FName Name) const;
	UTextBlock* FindTopBarText(const FName Name) const;
	void SetText(const FName Name, const FString& Value) const;
	void SetFleetText(const FName Name, const FString& Value) const;
	void SetTopBarText(const FName Name, const FString& Value) const;
	void RefreshFromFleet();
	void RefreshFleetRows(const TArray<FDroneData>& Drones, int32 TotalCount);
	void HandleFleetRowSelectionClicked(int32 RowIndex);
	void ApplyDroneData(const FDroneData& DroneData);
	void SubmitSelectedProductionCommand(
		EGCSProductionCommandType CommandType,
		const FGCSProductionCommandParameters& Parameters = {});
	TArray<FString> GetCommandTargetDroneIDs() const;
	void RefreshHoverMVPControls();
	void SetButtonLabel(FName ButtonName, const FString& Value) const;
	void SetFleetButtonLabel(FName ButtonName, const FString& Value) const;
	void SetLegacyHoverSelectionVisible(bool bVisible) const;
	void BindExistingButtons();
	void BindFleetPanelButtons();
	void BindNavigationButtons();
	void CreateNavigationPages();
	void ResetNavigationPages();
	void ShowMainPage(EGCSMainPage Page);
	void ApplyNavigationVisibility();
	UUserWidget* CreateNavigationPage(const TCHAR* ClassPath) const;
	static void SetWidgetVisible(const TWeakObjectPtr<UUserWidget>& Widget, bool bVisible);
	static void SetPageVisible(const TWeakObjectPtr<UUserWidget>& Widget, bool bVisible);

	TWeakObjectPtr<UUserWidget> MainWidgetRef;
	TWeakObjectPtr<UUserWidget> TopBarWidget;
	TWeakObjectPtr<UUserWidget> LeftToolbarWidget;
	TWeakObjectPtr<UUserWidget> FleetPanelWidget;
	TWeakObjectPtr<UUserWidget> DroneDetailWidget;
	TWeakObjectPtr<UUserWidget> MapPageWidget;
	TWeakObjectPtr<UUserWidget> SettingsPageWidget;
	TWeakObjectPtr<UUserWidget> LogsPageWidget;
	EGCSMainPage ActivePage = EGCSMainPage::Fleet;
	bool bPanelsExpanded = true;
	FString LastCommandResultText;
	TSet<FString> SelectedCommandDroneIDs;
	TArray<FString> CurrentFleetPageDroneIDs;
	int32 FleetPageOffset = 0;
	static constexpr int32 FleetPageSize = 5;
	float HoverAltitudeMeters = 5.0f;
};
