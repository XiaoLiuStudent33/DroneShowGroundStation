/**
 * @brief 将 Fleet/Command 状态映射到现有 UMG Widget 的 UI Bridge/ViewModel。
 *
 * 分类：Production。
 * 输入：FleetManager 事件与用户按钮；输出：Widget 文本/选择状态和 DSP Production 命令。
 * 关键约束：详情选择 CurrentSelectedDroneID 与多机命令集合 SelectedCommandDroneIDs 相互独立。
 */
#pragma once

#include "CoreMinimal.h"
#include "DSPGCSProductionCommandTypes.h"
#include "DroneData.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GCSUIBridgeSubsystem.generated.h"

class UButton;
class ACameraActor;
class APlayerController;
class UGCSDroneRowView;
class UGCSRouteWorkflowCoordinator;
class UEditableTextBox;
class USlider;
class UTextBlock;
class UUserWidget;
class UWidget;
class FGCSMarqueeInputProcessor;
struct FGeometry;
struct FPointerEvent;
class FReply;
enum class EDSPGCSTelemetryLinkState : uint8;

UENUM(BlueprintType)
enum class EGCSDroneDisplayStatus : uint8
{
	Online,
	Offline,
	LinkLost
};

enum class EGCSMainPage : uint8
{
	Fleet,
	Control,
	Map,
	Settings,
	Logs
};

DECLARE_MULTICAST_DELEGATE(FGCSCommandSelectionChanged);

UCLASS()
class DRONESHOWPLATFORM1_API UGCSUIBridgeSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UGCSUIBridgeSubsystem();
	void BindMainWidget(UUserWidget* MainWidget);
	virtual void Deinitialize() override;

	// Detail presentation interface. The authoritative state remains in
	// FleetManager; this method only copies one selected snapshot into UMG.
	UFUNCTION(BlueprintCallable, Category = "Drone GCS|UI")
	void UpdateDroneInfo(const FDroneData& DroneData);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|UI")
	void SetCommandDroneSelected(const FString& DroneID, bool bSelected);

	UFUNCTION(BlueprintPure, Category = "Drone GCS|UI")
	TArray<FString> GetSelectedCommandDroneIDs() const;

	/** Replaces the existing command target set without changing detail selection. */
	void ReplaceCommandDroneSelection(const TArray<FString>& DroneIDs);

	/** Native, once-per-authority-mutation notification used by bulk-selection consumers. */
	FGCSCommandSelectionChanged OnCommandSelectionChanged;

#if WITH_DEV_AUTOMATION_TESTS
	void ToggleFleetSelectAllForTest() { HandleFleetSelectAllClicked(); }
	void RefreshFleetSelectAllForTest() { RefreshFleetSelectAll(); }
	void RebindFleetPanelButtonsForTest() { BindFleetPanelButtons(); }
	TArray<FString> GetEligibleOnlineCommandDroneIDsForTest() const
	{
		return BuildEligibleOnlineCommandDroneIDs();
	}
#endif

	static EGCSDroneDisplayStatus ResolveDroneDisplayStatus(
		EDSPGCSTelemetryLinkState ServerState,
		bool bDroneOnline);

private:
	friend class FGCSMarqueeInputProcessor;

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
	void HandleTopDownLocateClicked();

	UFUNCTION()
	void HandleColorOffClicked();

	UFUNCTION()
	void HandleColorWhiteClicked();

	UFUNCTION()
	void HandleColorRedClicked();

	UFUNCTION()
	void HandleColorGreenClicked();

	UFUNCTION()
	void HandleColorBlueClicked();

	UFUNCTION()
	void HandleColorYellowClicked();

	UFUNCTION()
	void HandleColorMagentaClicked();

	UFUNCTION()
	void HandleRoutePreviousFrameClicked();

	UFUNCTION()
	void HandleRoutePauseClicked();

	UFUNCTION()
	void HandleRoutePlayClicked();

	UFUNCTION()
	void HandleRouteStopClicked();

	UFUNCTION()
	void HandleRouteNextFrameClicked();

	UFUNCTION()
	void HandleRouteExitPreviewClicked();

	UFUNCTION()
	void HandleRouteUnloadClicked();

	UFUNCTION()
	void HandleRouteTimelineValueChanged(float NormalizedValue);

	UFUNCTION()
	void HandleRouteTimelineCaptureBegin();

	UFUNCTION()
	void HandleRouteTimelineCaptureEnd();

	UFUNCTION()
	void HandleRouteImportClicked();

	UFUNCTION()
	void HandleRouteImportCloseClicked();

	UFUNCTION()
	void HandleRouteBrowseClicked();

	UFUNCTION()
	void HandleRouteLoadPreviewClicked();

	UFUNCTION()
	void HandleRouteSampleRateTextChanged(const FText& Text);

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
	void HandleFleetSelectAllClicked();

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
	void HandleSelectedDroneChanged(const FString& DroneID);

	UFUNCTION()
	void HandleFleetDataBatchApplied(int64 FleetRevision, int32 UpdatedDroneCount);

	UFUNCTION()
	void HandleTelemetryLinkStateChanged();

	UButton* FindButton(const FName Name) const;
	UButton* FindFleetButton(const FName Name) const;
	UButton* FindToolbarButton(const FName Name) const;
	UButton* FindRightPanelButton(const FName Name) const;
	UTextBlock* FindText(const FName Name) const;
	UTextBlock* FindFleetText(const FName Name) const;
	UTextBlock* FindTopBarText(const FName Name) const;
	void SetText(const FName Name, const FString& Value) const;
	void SetFleetText(const FName Name, const FString& Value) const;
	void SetFleetStatus(
		FName Name,
		const FString& Value,
		const FLinearColor& Color) const;
	void SetTopBarText(const FName Name, const FString& Value) const;
	void RefreshFromFleet();
	void RefreshProductionCommandAvailability();
	EDSPGCSTelemetryLinkState GetProductionTelemetryLinkState() const;
	void RefreshFleetRows(
		const TArray<FDroneData>& Drones,
		int32 TotalCount,
		const FString& CurrentSelectedDroneID);
	void RefreshFleetSelectAll();
	TArray<FString> BuildEligibleOnlineCommandDroneIDs() const;
	bool IsExactCommandSelection(const TArray<FString>& EligibleDroneIDs) const;
	bool IsCommandSelectionMutationBlocked() const;
	FReply HandleFleetRowMouseButtonDown(
		const FGeometry& Geometry,
		const FPointerEvent& MouseEvent,
		int32 RowIndex);
	void HandleFleetRowDetailClicked(int32 RowIndex);
	void HandleFleetRowSelectionClicked(int32 RowIndex);
	void InitializeFleetRowViews();
	void UpdateFleetRowView(
		int32 RowIndex,
		const FString& DroneID,
		const FString& CurrentSelectedDroneID);
	void SubmitSelectedProductionCommand(
		EGCSProductionCommandType CommandType,
		const FGCSProductionCommandParameters& Parameters = {});
	TArray<FString> GetCommandTargetDroneIDs() const;
	void SyncCommandSelectionToVisualization() const;
	void RefreshHoverMVPControls();
	void SetFleetButtonLabel(FName ButtonName, const FString& Value) const;
	void SetLegacyHoverSelectionVisible(bool bVisible) const;
	void RegisterMarqueeInputProcessor();
	void UnregisterMarqueeInputProcessor();
	void PollMarqueePlayerControllerInput();
	void TickRoutePlaybackUI(float DeltaSeconds);
	void BeginMarqueeCameraLock(APlayerController* PlayerController);
	void EndMarqueeCameraLock();
	void UpdateTopDownViewInput(APlayerController* PlayerController);
	void ResetTopDownViewInputState();
	void UpdateTopDownLocateLock(APlayerController* PlayerController);
	void EndTopDownLocateLock(APlayerController* PlayerController = nullptr);
	void HandleMarqueeMouseButtonDown(const FPointerEvent& MouseEvent);
	void HandleMarqueeMouseMove(const FPointerEvent& MouseEvent);
	void HandleMarqueeMouseButtonUp(const FPointerEvent& MouseEvent);
	bool IsPointerOverBlockingGCSUI(const FVector2D& ScreenPosition) const;
	FVector2D ScreenToMainWidgetLocal(const FVector2D& ScreenPosition) const;
	void UpdateSelectionRectangleVisual(
		const FVector2D& StartPosition,
		const FVector2D& CurrentPosition,
		bool bVisible) const;
	void CompleteMarqueeSelection(
		const FVector2D& StartPosition,
		const FVector2D& EndPosition);
	void BindExistingButtons();
	void BindColorPaletteButtons();
	void BindRoutePlaybackControls();
	bool BindRouteImportControls(UUserWidget* RouteImportWidget);
	void OpenRouteImport();
	void CloseRouteImport();
	void RefreshRouteImportMetadata();
	void RefreshRoutePlaybackUI();
	void StepRoutePreviewFrame(int32 Direction);
	void ApplySelectedDronePreviewColor(const FLinearColor& Color) const;
	void ApplyAndSubmitSelectedDroneLedColor(
		const FLinearColor& PreviewColor,
		uint8 Red,
		uint8 Green,
		uint8 Blue);
	void BindFleetPanelButtons();
	void BindNavigationButtons();
	void CreateNavigationPages();
	void ResetNavigationPages();
	void ShowMainPage(EGCSMainPage Page);
	void ApplyNavigationVisibility();
	void ApplyNavigationPageVisibility();
	UUserWidget* CreateNavigationPage(const TCHAR* ClassPath) const;

	TWeakObjectPtr<UUserWidget> MainWidgetRef;
	UPROPERTY()
	TSubclassOf<UUserWidget> RouteImportWidgetClass;
	TWeakObjectPtr<UUserWidget> TopBarWidget;
	TWeakObjectPtr<UUserWidget> LeftToolbarWidget;
	TWeakObjectPtr<UUserWidget> FleetPanelWidget;
	TWeakObjectPtr<UUserWidget> DroneDetailWidget;
	TWeakObjectPtr<UUserWidget> RightToolbarWidget;
	TWeakObjectPtr<UUserWidget> RightPanelWidget;
	TWeakObjectPtr<UUserWidget> PlaybackWidget;
	TWeakObjectPtr<UUserWidget> RouteImportWidget;
	TWeakObjectPtr<UButton> RouteImportButton;
	TWeakObjectPtr<UButton> RouteImportCloseButton;
	TWeakObjectPtr<UButton> RouteBrowseButton;
	TWeakObjectPtr<UButton> RouteCancelButton;
	TWeakObjectPtr<UButton> RouteLoadPreviewButton;
	TWeakObjectPtr<UEditableTextBox> RouteSampleRateInput;
	TWeakObjectPtr<UEditableTextBox> RouteOriginLatitudeInput;
	TWeakObjectPtr<UEditableTextBox> RouteOriginLongitudeInput;
	TWeakObjectPtr<UEditableTextBox> RouteOriginAltitudeInput;
	TWeakObjectPtr<UTextBlock> RouteImportFilePathText;
	TWeakObjectPtr<UTextBlock> RouteImportDroneValueText;
	TWeakObjectPtr<UTextBlock> RouteImportFrameCountValueText;
	TWeakObjectPtr<UTextBlock> RouteImportDurationValueText;
	TWeakObjectPtr<UButton> RoutePreviousButton;
	TWeakObjectPtr<UButton> RoutePauseButton;
	TWeakObjectPtr<UButton> RoutePlayButton;
	TWeakObjectPtr<UButton> RouteStopButton;
	TWeakObjectPtr<UButton> RouteNextButton;
	TWeakObjectPtr<UButton> RouteExitPreviewButton;
	TWeakObjectPtr<UButton> RouteUnloadButton;
	TWeakObjectPtr<USlider> RouteTimelineSlider;
	TWeakObjectPtr<UTextBlock> RouteTimeText;
	TWeakObjectPtr<UTextBlock> RouteFrameText;
	TWeakObjectPtr<UWidget> SelectionRectangleWidget;
	TWeakObjectPtr<UUserWidget> MapPageWidget;
	TWeakObjectPtr<UUserWidget> SettingsPageWidget;
	TWeakObjectPtr<UUserWidget> LogsPageWidget;
	EGCSMainPage ActivePage = EGCSMainPage::Fleet;
	bool bPanelsExpanded = true;
	FString LastCommandResultText;
	FString SelectedRouteImportFilePath;
	TSet<FString> SelectedCommandDroneIDs;
	TArray<FString> CurrentFleetPageDroneIDs;
	TSharedPtr<FGCSMarqueeInputProcessor> MarqueeInputProcessor;
	TWeakObjectPtr<APlayerController> MarqueeCameraLockPlayerController;
	FVector2D MarqueeStartScreenPosition = FVector2D::ZeroVector;
	FVector2D MarqueeStartLocalPosition = FVector2D::ZeroVector;
	FVector2D MarqueeCurrentLocalPosition = FVector2D::ZeroVector;
	bool bMarqueePointerDown = false;
	bool bMarqueeSelecting = false;
	bool bMarqueeLeftMouseWasDown = false;
	bool bMarqueeCameraLocked = false;
	bool bTopDownLocateLocked = false;
	bool bTopDownMiddleMouseWasDown = false;
	bool bTopDownPanning = false;
	bool bUpdatingRouteTimelineFromState = false;
	float RouteUIRefreshAccumulator = 0.0f;
	FVector2D TopDownLastMousePosition = FVector2D::ZeroVector;
	float TopDownFitOrthoWidth = 0.0f;
	FVector TopDownLockedCameraLocation = FVector::ZeroVector;
	FRotator TopDownLockedCameraRotation = FRotator::ZeroRotator;
	FVector TopDownLocalEast = FVector::XAxisVector;
	FVector TopDownLocalNorth = -FVector::YAxisVector;
	FVector TopDownLocalUp = FVector::ZAxisVector;
	bool bTopDownGeographicBasisActive = false;
	TWeakObjectPtr<ACameraActor> TopDownOrthographicCamera;
	TWeakObjectPtr<AActor> PreviousTopDownViewTarget;
	TWeakObjectPtr<APlayerController> TopDownPlayerController;
	FRotator PreviousTopDownControlRotation = FRotator::ZeroRotator;
	bool bHasPreviousTopDownControlRotation = false;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UGCSDroneRowView>> FleetRowViews;

	UPROPERTY(Transient)
	TObjectPtr<UGCSRouteWorkflowCoordinator> RouteWorkflowCoordinator;

	int32 FleetPageOffset = 0;
	static constexpr int32 FleetPageSize = 5;
	static constexpr float MarqueeDragThresholdPixels = 5.0f;
	static constexpr float TopDownZoomFactor = 0.85f;
	static constexpr float TopDownMinOrthoWidthFactor = 0.03f;
	static constexpr float TopDownMaxOrthoWidthFactor = 8.0f;
	static constexpr float RouteUIRefreshIntervalSeconds = 0.1f;
	float HoverAltitudeMeters = 5.0f;
};
