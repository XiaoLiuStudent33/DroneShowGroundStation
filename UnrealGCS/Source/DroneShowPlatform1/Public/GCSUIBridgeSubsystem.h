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
class AActor;
class ADroneFleetVisualizationActor;
class APlayerController;
class UGCSFleetRowCheckBinding;
class UGCSRouteWorkflowCoordinator;
class UGCSTelemetryFilterController;
class UEditableTextBox;
class UEditableText;
class USlider;
class USpinBox;
class UCheckBox;
class UComboBoxString;
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
	bool IsCommandSelectionMutationBlocked() const;

#if WITH_DEV_AUTOMATION_TESTS
	void SelectAllEligibleFleetForTest() { HandleFleetSelectAllCheckStateChanged(true); }
	void RefreshFleetSelectAllForTest() { RefreshFleetSelectAll(); }
	void RebindFleetPanelButtonsForTest() { BindFleetPanelButtons(); }
	void BindUploadDashboardForTest(UUserWidget* MainWidget, UUserWidget* RightToolbar);
	void SetUploadDashboardFactoryForTest(TFunction<UUserWidget*()> Factory);
	void ToggleUploadDashboardForTest() { HandleUploadDashboardToggleClicked(); }
	void ResetUploadDashboardForTest() { ResetUploadDashboard(); }
	UUserWidget* GetUploadDashboardForTest() const { return UploadDashboardWidget; }
	UButton* GetUploadDashboardToggleButtonForTest() const
	{
		return UploadDashboardToggleButton.Get();
	}
	bool IsUploadDashboardToggleBoundForTest() const;
	bool IsUploadDashboardVisibleForTest() const { return bUploadDashboardVisible; }
	int32 GetUploadDashboardToggleCountForTest() const
	{
		return UploadDashboardToggleCountForTest;
	}
	void RefreshUploadDashboardForTest() { RefreshUploadDashboardUI(); }
	void AutoCapUploadDashboardForTest() { HandleUploadDashboardAutoCapClicked(); }
	void RestoreUploadDashboardForTest() { HandleUploadDashboardRestoreClicked(); }
	int32 GetUploadDashboardDraftGlobalCapForTest() const
	{
		return UploadDashboardDraftGlobalCap;
	}
	bool IsUploadDashboardDraftDirtyForTest() const
	{
		return bUploadDashboardDraftDirty;
	}
	FString GetUploadDashboardTuningStatusForTest() const
	{
		return UploadDashboardTuningStatus;
	}
	TArray<FString> GetEligibleOnlineCommandDroneIDsForTest() const
	{
		return BuildEligibleOnlineCommandDroneIDs();
	}
#endif

	static EGCSDroneDisplayStatus ResolveDroneDisplayStatus(
		EDSPGCSTelemetryLinkState ServerState,
		bool bDroneOnline);
	bool IsPointerOverBlockingGCSUI(const FVector2D& ScreenPosition) const;

private:
	friend class FGCSMarqueeInputProcessor;
	friend class UGCSFleetRowCheckBinding;
	friend class FGCSIndependentToolbarTogglesTest;
	friend class FGCSScheduledShowPreviewTest;
	friend class FGCSTelemetryFilterToggleTest;

	UFUNCTION()
	void HandleTelemetryFilterToggleClicked();
	void BindTelemetryFilterToggle();
	void ResetTelemetryFilter();

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
	void HandleHoverPanelToggleClicked();

	UFUNCTION()
	void HandleHoverGenerateLayersClicked();

	UFUNCTION()
	void HandleHoverLayerSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleHoverAltitudeCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleHoverUnlockClicked();

	UFUNCTION()
	void HandleHoverTakeoffClicked();

	UFUNCTION()
	void HandleHoverLandClicked();

	UFUNCTION()
	void HandleHoverLockClicked();

	UFUNCTION()
	void HandleHoverManualClicked();

	UFUNCTION()
	void HandleStopClicked();

	UFUNCTION()
	void HandleFleetSelectAllCheckStateChanged(bool bIsChecked);

	UFUNCTION()
	void HandleHoverAltitudeMinusClicked();

	UFUNCTION()
	void HandleHoverAltitudePlusClicked();

	UFUNCTION()
	void HandleManualModeClicked();

	UFUNCTION()
	void HandleAutonomousModeClicked();

	UFUNCTION()
	void HandleTimeStartClicked();

	UFUNCTION()
	void HandleTimeStopClicked();

	UFUNCTION()
	void HandleExecuteDanceStepClicked();

	UFUNCTION()
	void HandleEndDanceStepClicked();

	UFUNCTION()
	void HandleFixedPileApplyClicked();

	UFUNCTION()
	void HandleBenchmarkExportClicked();

	UFUNCTION()
	void HandleMapCheckStateChanged(bool bIsChecked);

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

	UFUNCTION()
	void HandleUploadDashboardToggleClicked();

	UFUNCTION()
	void HandleUploadDashboardGlobalCapTextChanged(const FText& Text);

	UFUNCTION()
	void HandleUploadDashboardApplyClicked();

	UFUNCTION()
	void HandleUploadDashboardAutoCapClicked();

	UFUNCTION()
	void HandleUploadDashboardRestoreClicked();

	UButton* FindButton(const FName Name) const;
	UButton* FindToolbarButton(const FName Name) const;
	UButton* FindRightPanelButton(const FName Name) const;
	USpinBox* FindRightPanelSpinBox(const FName Name) const;
	UCheckBox* FindRightPanelCheckBox(const FName Name) const;
	UTextBlock* FindRightPanelText(const FName Name) const;
	UWidget* FindRightPanelWidget(const FName Name) const;
	UTextBlock* FindText(const FName Name) const;
	UTextBlock* FindFleetText(const FName Name) const;
	UTextBlock* FindTopBarText(const FName Name) const;
	void SetText(const FName Name, const FString& Value) const;
	void SetFleetText(const FName Name, const FString& Value) const;
	void SetTopBarText(const FName Name, const FString& Value) const;
	void SetRightPanelText(const FName Name, const FString& Value) const;
	void RefreshFromFleet();
	void RefreshProductionCommandAvailability();
	EDSPGCSTelemetryLinkState GetProductionTelemetryLinkState() const;
	void RefreshFleetListRows(
		const TArray<FDroneData>& Drones,
		int32 TotalCount,
		const FString& CurrentSelectedDroneID);
	void RefreshFleetSelectAll();
	void UnbindFleetList();
	void HandleFleetListEntryGenerated(UUserWidget& Entry);
	void HandleFleetListEntryReleased(UUserWidget& Entry);
	void HandleFleetListCommandCheckChanged(UObject* RowObject, bool bIsChecked);
	void HandleFleetListDetailClicked(UObject* RowObject);
	TArray<FString> BuildEligibleOnlineCommandDroneIDs() const;
	bool IsExactCommandSelection(const TArray<FString>& EligibleDroneIDs) const;
	FString SubmitSelectedProductionCommand(
		EGCSProductionCommandType CommandType,
		const FGCSProductionCommandParameters& Parameters = {});
	FString SubmitProductionCommandForTargets(
		EGCSProductionCommandType CommandType,
		const TArray<FString>& TargetDroneIDs,
		const FGCSProductionCommandParameters& Parameters = {});
	TArray<FString> GetCommandTargetDroneIDs() const;
	void SyncCommandSelectionToVisualization() const;
	void RefreshHoverMVPControls();
	void BindHoverTestControls();
	void UnbindHoverTestControls();
	void RefreshHoverTestControls();
	void ResetHoverLayerPlan(const FString& Status = {});
	void HandleHoverCommandSelectionChanged();
	bool BuildHoverPositions(
		const TArray<FString>& DroneIDs,
		TArray<struct FHoverLayerPosition>& OutPositions,
		FString& OutError,
		FString* OutPositionSource = nullptr) const;
	bool TryReadHoverAltitude(FString& OutError);
	bool PrepareHoverLayerAction(FString& OutError, bool bRequireCurrentSpacing);
	void ApplyHoverLayerSelection(int32 ComboOptionIndex);
	void SetLegacyHoverSelectionVisible(bool bVisible) const;
	void RegisterMarqueeInputProcessor();
	void UnregisterMarqueeInputProcessor();
	void PollMarqueePlayerControllerInput();
	void TickRoutePlaybackUI(float DeltaSeconds);
	void TickTimeControlUI(float DeltaSeconds);
	void ResetTimeControlPreparationState();
	void RefreshTimeControlPreparationStatus(const FString& Prefix = {});
	void BeginMarqueeCameraLock(APlayerController* PlayerController);
	void EndMarqueeCameraLock();
	void UpdateTopDownViewInput(APlayerController* PlayerController);
	void ResetTopDownViewInputState();
	void UpdateTopDownLocateLock(APlayerController* PlayerController);
	void EndTopDownLocateLock(APlayerController* PlayerController = nullptr);
	void HandleMarqueeMouseButtonDown(const FPointerEvent& MouseEvent);
	void HandleMarqueeMouseMove(const FPointerEvent& MouseEvent);
	void HandleMarqueeMouseButtonUp(const FPointerEvent& MouseEvent);
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
	void BindTimeControlControls();
	void BindReferenceAndMapControls();
	void UnbindReferenceAndMapControls();
	ADroneFleetVisualizationActor* FindFleetVisualizationActor() const;
	bool InitializeCoordinateAxes(
		const FVector& OriginWorld,
		const FVector& EastWorld,
		const FVector& NorthWorld,
		int32 OriginDroneNumber,
		float AxisLengthMeters) const;
	TArray<AActor*> FindSceneMapActors() const;
	void SetSceneMapVisible(bool bVisible) const;
	void SyncMapCheckBoxFromScene();
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
	void ConfigureGroundStationInputPolicy(UUserWidget* MainWidget) const;
	void ConfigureTelemetryTableInput() const;
	void UpdateTelemetryHorizontalScrollInput(APlayerController* PlayerController);
	void ResetTelemetryHorizontalDrag();
	void TickTelemetryTableUI(float DeltaSeconds);
	void RefreshTelemetryTableFromProduction();
	void ResetNavigationPages();
	void ApplyNavigationVisibility();
	UUserWidget* CreateNavigationPage(const TCHAR* ClassPath) const;
	void BindUploadDashboardControls();
	void UnbindUploadDashboardControls();
	bool CreateUploadDashboardIfNeeded();
	bool AttachUploadDashboardToMainLayout();
	void ApplyUploadDashboardVisibility();
	void ResetUploadDashboard();
	void BindUploadDashboardDataControls();
	void UnbindUploadDashboardDataControls();
	void TickUploadDashboardUI(float DeltaSeconds);
	void RefreshUploadDashboardUI();
	bool GetUploadDashboardTargetCount(int32& OutTargetCount) const;
	bool IsUploadDashboardMutationLocked() const;
	void SetUploadDashboardStatus(const FString& Status, const FString& Diagnostic = {});

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
	TWeakObjectPtr<UButton> UploadDashboardToggleButton;
	TWeakObjectPtr<UButton> UploadDashboardApplyButton;
	TWeakObjectPtr<UButton> UploadDashboardAutoCapButton;
	TWeakObjectPtr<UButton> UploadDashboardRestoreButton;
	TWeakObjectPtr<UWidget> UploadDashboardWriteWindowInput;
	TWeakObjectPtr<UWidget> UploadDashboardGlobalCapInput;
	TWeakObjectPtr<UWidget> UploadDashboardSegmentItemsInput;
	TWeakObjectPtr<UWidget> UploadDashboardRequestTimeoutInput;
	TWeakObjectPtr<UWidget> UploadDashboardTerminateTimeoutInput;
	TWeakObjectPtr<UWidget> UploadDashboardRetryCountInput;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> UploadDashboardWidget;

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
	TWeakObjectPtr<UUserWidget> LogsPageWidget;
	bool bPanelsExpanded = true;
	bool bFleetPanelRequestedVisible = false;
	bool bDroneDetailRequestedVisible = false;
	FString LastCommandResultText;
	FString SelectedRouteImportFilePath;
	TSet<FString> SelectedCommandDroneIDs;
	TWeakObjectPtr<class UListView> FleetListView;
	TWeakObjectPtr<UCheckBox> FleetSelectAllCheckBox;
	TMap<TWeakObjectPtr<UUserWidget>, TWeakObjectPtr<UGCSFleetRowCheckBinding>>
		FleetListEntryBindings;
	bool bUpdatingFleetList = false;
	FString LastRenderedFleetDetailDroneID;
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
	bool bTelemetryRightMouseWasDown = false;
	bool bTelemetryHorizontalDragging = false;
	FVector2D TelemetryHorizontalDragLastLocalPosition = FVector2D::ZeroVector;
	bool bUpdatingRouteTimelineFromState = false;
	bool bUpdatingMapCheckBoxFromScene = false;
	float RouteUIRefreshAccumulator = 0.0f;
	float TimeControlUIRefreshAccumulator = 0.0f;
	bool bTimeControlCountdownActive = false;
	FDateTime ScheduledTimeLocal;
	TArray<FString> TimeControlTargetDroneIDs;
	TSet<FString> TimeControlScheduleSentDroneIDs;
	TSet<FString> TimeControlFailedDroneIDs;
	FString PendingTimeControlCommandID;
	EGCSProductionCommandType PendingTimeControlCommandType =
		EGCSProductionCommandType::TimeStart;
	int32 PendingTimeControlTargetCount = 0;
	int32 PendingTimeControlResultCount = 0;
	int32 PendingTimeControlSucceededCount = 0;
	TSet<FString> PendingTimeControlCompletedDroneIDs;
	FVector2D TopDownLastMousePosition = FVector2D::ZeroVector;
	float TopDownFitOrthoWidth = 0.0f;
	FVector TopDownLockedCameraLocation = FVector::ZeroVector;
	FRotator TopDownLockedCameraRotation = FRotator::ZeroRotator;
	FVector TopDownLocalEast = FVector::XAxisVector;
	FVector TopDownLocalNorth = -FVector::YAxisVector;
	FVector TopDownLocalUp = FVector::ZAxisVector;
	bool bTopDownGeographicBasisActive = false;
	bool bUploadDashboardVisible = false;
	bool bUploadDashboardDataBound = false;
	bool bUploadDashboardDraftDirty = false;
	bool bUploadDashboardApplyPending = false;
	bool bUpdatingUploadDashboardDraft = false;
	int32 UploadDashboardDraftGlobalCap = 0;
	uint64 UploadDashboardObservedConfigRevision = 0;
	FString UploadDashboardObservedAuthoritySessionID;
	FString UploadDashboardTuningStatus = TEXT("NOT CONNECTED");
	FString UploadDashboardTuningDiagnostic;
	float UploadDashboardRefreshAccumulator = 0.0f;
	float TelemetryTableRefreshAccumulator = 0.0f;
	TWeakObjectPtr<ACameraActor> TopDownOrthographicCamera;
	TWeakObjectPtr<AActor> PreviousTopDownViewTarget;
	TWeakObjectPtr<APlayerController> TopDownPlayerController;
	FRotator PreviousTopDownControlRotation = FRotator::ZeroRotator;
	bool bHasPreviousTopDownControlRotation = false;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UObject>> FleetListRowObjects;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UGCSFleetRowCheckBinding>> FleetListCheckBindings;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UObject>> TelemetryRowObjects;

	UPROPERTY(Transient)
	TObjectPtr<UGCSRouteWorkflowCoordinator> RouteWorkflowCoordinator;

	UPROPERTY(Transient)
	TObjectPtr<UGCSTelemetryFilterController> TelemetryFilterController;
	TWeakObjectPtr<UUserWidget> TelemetryFilterWidget;
	TWeakObjectPtr<UButton> TelemetryFilterToggleButton;
	TWeakObjectPtr<UWidget> HoverTestContentWidget;
	TWeakObjectPtr<UEditableTextBox> HoverAltitudeInput;
	TWeakObjectPtr<UComboBoxString> HoverLayerCombo;
	TWeakObjectPtr<UButton> HoverGenerateLayersButton;
	TArray<TArray<FString>> HoverLayers;
	TArray<FString> HoverLayerBaseDroneIDs;
	TArray<FString> HoverLayerOptions;
	FString HoverLayerFleetSessionID;
	FString HoverLayerPositionSource;
	FString HoverTestStatus;
	int32 ActiveHoverLayerOptionIndex = INDEX_NONE;
	bool bUpdatingHoverLayerCombo = false;
	bool bApplyingHoverLayerSelection = false;

#if WITH_DEV_AUTOMATION_TESTS
	TFunction<UUserWidget*()> UploadDashboardFactoryForTest;
	int32 UploadDashboardToggleCountForTest = 0;
#endif

	static constexpr float MarqueeDragThresholdPixels = 5.0f;
	static constexpr float TopDownZoomFactor = 0.85f;
	static constexpr float TopDownMinOrthoWidthFactor = 0.03f;
	static constexpr float TopDownMaxOrthoWidthFactor = 8.0f;
	static constexpr float RouteUIRefreshIntervalSeconds = 0.1f;
	static constexpr float TimeControlUIRefreshIntervalSeconds = 0.1f;
	static constexpr float UploadDashboardRefreshIntervalSeconds = 0.2f;
	static constexpr float TelemetryTableRefreshIntervalSeconds = 1.0f;
	float HoverAltitudeMeters = 5.0f;
	static constexpr double HoverLayerMinimumSpacingMeters = 0.6;
};
