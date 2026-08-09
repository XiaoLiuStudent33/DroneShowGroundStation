/**
 * @brief 实现主 UI 绑定、Fleet 分页、详情显示、命令选择和页面导航。
 *
 * 分类：Production。
 * 当前命令只调用 DSPGCSProductionCommandProviderSubsystem。
 */
#include "GCSUIBridgeSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "DSPGCSProductionCommandProviderSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "FleetManagerSubsystem.h"
#include "GCSDroneRowView.h"
#include "GCSUIWidgetPresenter.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWidget.h"

void UGCSUIBridgeSubsystem::BindMainWidget(UUserWidget* MainWidget)
{
	ResetNavigationPages();

	MainWidgetRef = MainWidget;
	TopBarWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_TopBar"));
	LeftToolbarWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_LeftToolbar"));
	FleetPanelWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_FleetPanel"));
	UUserWidget* RightToolbar =
		FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_RightToolbar"));
	TWeakObjectPtr<UUserWidget> RightPanel =
		FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_RightPanel"));
	UUserWidget* DroneDetail = FGCSUIWidgetPresenter::ResolveDroneDetail(MainWidget);

	if (!DroneDetail)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("WBP_GCS_DroneDetail was not found in the active main widget."));
		return;
	}

	DroneDetailWidget = DroneDetail;
	CreateNavigationPages();
	BindNavigationButtons();
	ShowMainPage(EGCSMainPage::Fleet);
	FGCSUIWidgetPresenter::SetWidgetVisible(RightPanel, false);

	if (UButton* ViewButton =
		FGCSUIWidgetPresenter::FindButton(RightToolbar, TEXT("BTN_View")))
	{
		StaticCastSharedRef<SButton>(ViewButton->TakeWidget())->SetOnClicked(
			FOnClicked::CreateLambda(
				[RightPanel]()
				{
					if (UUserWidget* Panel = RightPanel.Get())
					{
						Panel->SetVisibility(
							Panel->GetVisibility() == ESlateVisibility::Collapsed
								? ESlateVisibility::Visible
								: ESlateVisibility::Collapsed);
					}
					return FReply::Handled();
				}));
	}

	// Preserve the authored DroneDetail layout. The hover altitude and ground
	// station mode controls remain in this asset, while fleet selection now
	// belongs to WBP_GCS_FleetPanel.
	if (UCanvasPanelSlot* DetailSlot = Cast<UCanvasPanelSlot>(DroneDetail->Slot))
	{
		FVector2D DetailSize = DetailSlot->GetSize();

		if (DetailSize.Y < 500.0f)
		{
			DetailSize.Y = 500.0f;
			DetailSlot->SetSize(DetailSize);
		}
	}

	BindExistingButtons();
	BindFleetPanelButtons();
	SetLegacyHoverSelectionVisible(false);
	RefreshHoverMVPControls();

	if (UDSPGCSProductionCommandProviderSubsystem* Commands =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>())
	{
		Commands->OnCommandResult.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleProductionCommandResult);
		Commands->OnCommandResult.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleProductionCommandResult);
	}
	if (UDSPGCSProductionTelemetryProviderSubsystem* Telemetry =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>())
	{
		Telemetry->OnTelemetryLinkStateChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleTelemetryLinkStateChanged);
		Telemetry->OnTelemetryLinkStateChanged.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleTelemetryLinkStateChanged);
	}

	if (UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>())
	{
		Fleet->OnFleetChanged.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetChanged);
		Fleet->OnFleetChanged.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetChanged);
		Fleet->OnDroneUpdated.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleDroneDataUpdated);
		Fleet->OnDroneUpdated.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleDroneDataUpdated);
		Fleet->OnDroneSelected.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleSelectedDroneChanged);
		Fleet->OnDroneSelected.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleSelectedDroneChanged);
		Fleet->OnDataBatchApplied.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetDataBatchApplied);
		Fleet->OnDataBatchApplied.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetDataBatchApplied);
		RefreshFromFleet();
	}
}

void UGCSUIBridgeSubsystem::Deinitialize()
{
	if (UDSPGCSProductionCommandProviderSubsystem* Commands =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>())
	{
		Commands->OnCommandResult.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleProductionCommandResult);
	}
	if (UDSPGCSProductionTelemetryProviderSubsystem* Telemetry =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>())
	{
		Telemetry->OnTelemetryLinkStateChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleTelemetryLinkStateChanged);
	}
	if (UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>())
	{
		Fleet->OnFleetChanged.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetChanged);
		Fleet->OnDroneUpdated.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleDroneDataUpdated);
		Fleet->OnDroneSelected.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleSelectedDroneChanged);
		Fleet->OnDataBatchApplied.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetDataBatchApplied);
	}
	ResetNavigationPages();
	MainWidgetRef.Reset();
	TopBarWidget.Reset();
	LeftToolbarWidget.Reset();
	FleetPanelWidget.Reset();
	DroneDetailWidget.Reset();
	LastCommandResultText.Reset();
	SelectedCommandDroneIDs.Reset();
	CurrentFleetPageDroneIDs.Reset();
	FleetRowViews.Reset();
	FleetPageOffset = 0;

	Super::Deinitialize();
}

EGCSDroneDisplayStatus UGCSUIBridgeSubsystem::ResolveDroneDisplayStatus(
	const EDSPGCSTelemetryLinkState ServerState,
	const bool bDroneOnline)
{
	if (ServerState == EDSPGCSTelemetryLinkState::Disconnected)
	{
		return EGCSDroneDisplayStatus::LinkLost;
	}
	return bDroneOnline
		? EGCSDroneDisplayStatus::Online
		: EGCSDroneDisplayStatus::Offline;
}

void UGCSUIBridgeSubsystem::HandleMenuClicked()
{
	bPanelsExpanded = !bPanelsExpanded;
	ApplyNavigationVisibility();
}

void UGCSUIBridgeSubsystem::HandleFleetPageClicked()
{
	ShowMainPage(EGCSMainPage::Fleet);
}

void UGCSUIBridgeSubsystem::HandleControlPageClicked()
{
	ShowMainPage(EGCSMainPage::Control);
}

void UGCSUIBridgeSubsystem::HandleMapPageClicked()
{
	ShowMainPage(EGCSMainPage::Map);
}

void UGCSUIBridgeSubsystem::HandleSettingsPageClicked()
{
	ShowMainPage(EGCSMainPage::Settings);
}

void UGCSUIBridgeSubsystem::HandleLogsPageClicked()
{
	ShowMainPage(EGCSMainPage::Logs);
}

void UGCSUIBridgeSubsystem::HandleArmClicked()
{
	SubmitSelectedProductionCommand(EGCSProductionCommandType::Arm);
}

void UGCSUIBridgeSubsystem::HandleDisarmClicked()
{
	SubmitSelectedProductionCommand(EGCSProductionCommandType::Disarm);
}

void UGCSUIBridgeSubsystem::HandleGuidedClicked()
{
	const TArray<FString> Targets = GetCommandTargetDroneIDs();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[UI] BTN_Guided Click Targets=%d [%s] Mode=GUIDED custom_mode=4"),
		Targets.Num(),
		*FString::Join(Targets, TEXT(",")));
	FGCSProductionCommandParameters Parameters;
	Parameters.CustomMode = 4;
	Parameters.ModeName = TEXT("GUIDED");
	SubmitSelectedProductionCommand(
		EGCSProductionCommandType::SetMode,
		Parameters);
}

void UGCSUIBridgeSubsystem::HandleTakeoffClicked()
{
	FGCSProductionCommandParameters Parameters;
	Parameters.TakeoffAltitudeMeters = 1.0f;
	SubmitSelectedProductionCommand(
		EGCSProductionCommandType::Takeoff,
		Parameters);
}

void UGCSUIBridgeSubsystem::HandleRTLClicked()
{
	SubmitSelectedProductionCommand(EGCSProductionCommandType::RTL);
}

void UGCSUIBridgeSubsystem::HandleLandClicked()
{
	SubmitSelectedProductionCommand(EGCSProductionCommandType::Land);
}

void UGCSUIBridgeSubsystem::HandleHoverClicked()
{
	FGCSProductionCommandParameters Parameters;
	Parameters.HoverAltitudeMeters = HoverAltitudeMeters;
	Parameters.HoverDurationSeconds = 0.0f;
	SubmitSelectedProductionCommand(
		EGCSProductionCommandType::HoverTest,
		Parameters);
}

void UGCSUIBridgeSubsystem::HandleFleetRow1SelectionClicked()
{
	HandleFleetRowSelectionClicked(0);
}

void UGCSUIBridgeSubsystem::HandleFleetRow2SelectionClicked()
{
	HandleFleetRowSelectionClicked(1);
}

void UGCSUIBridgeSubsystem::HandleFleetRow3SelectionClicked()
{
	HandleFleetRowSelectionClicked(2);
}

void UGCSUIBridgeSubsystem::HandleFleetRow4SelectionClicked()
{
	HandleFleetRowSelectionClicked(3);
}

void UGCSUIBridgeSubsystem::HandleFleetRow5SelectionClicked()
{
	HandleFleetRowSelectionClicked(4);
}

void UGCSUIBridgeSubsystem::HandleFleetPreviousPageClicked()
{
	FleetPageOffset = FMath::Max(0, FleetPageOffset - FleetPageSize);
	RefreshFromFleet();
}

void UGCSUIBridgeSubsystem::HandleFleetNextPageClicked()
{
	if (const UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>())
	{
		if (FleetPageOffset + FleetPageSize < Fleet->GetDroneCount())
		{
			FleetPageOffset += FleetPageSize;
		}
	}
	RefreshFromFleet();
}

void UGCSUIBridgeSubsystem::HandleHoverAltitudeMinusClicked()
{
	HoverAltitudeMeters = FMath::Clamp(HoverAltitudeMeters - 0.5f, 0.5f, 120.0f);
	RefreshHoverMVPControls();
}

void UGCSUIBridgeSubsystem::HandleHoverAltitudePlusClicked()
{
	HoverAltitudeMeters = FMath::Clamp(HoverAltitudeMeters + 0.5f, 0.5f, 120.0f);
	RefreshHoverMVPControls();
}

void UGCSUIBridgeSubsystem::HandleManualModeClicked()
{
	if (UDSPGCSProductionCommandProviderSubsystem* Commands =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>())
	{
		Commands->SetControlMode(EGCSGroundStationControlMode::Manual);
	}
	const TArray<FString> Targets = GetCommandTargetDroneIDs();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[UI] BTN_ModeManual Click Targets=%d [%s] Mode=STABILIZE custom_mode=0"),
		Targets.Num(),
		*FString::Join(Targets, TEXT(",")));
	FGCSProductionCommandParameters Parameters;
	Parameters.CustomMode = 0;
	Parameters.ModeName = TEXT("STABILIZE");
	SubmitSelectedProductionCommand(EGCSProductionCommandType::SetMode, Parameters);
	RefreshHoverMVPControls();
}

void UGCSUIBridgeSubsystem::HandleAutonomousModeClicked()
{
	if (UDSPGCSProductionCommandProviderSubsystem* Commands =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>())
	{
		Commands->SetControlMode(EGCSGroundStationControlMode::Autonomous);
	}
	const TArray<FString> Targets = GetCommandTargetDroneIDs();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[UI] BTN_ModeAutonomous Click Targets=%d [%s] Mode=AUTO custom_mode=3"),
		Targets.Num(),
		*FString::Join(Targets, TEXT(",")));
	FGCSProductionCommandParameters Parameters;
	Parameters.CustomMode = 3;
	Parameters.ModeName = TEXT("AUTO");
	SubmitSelectedProductionCommand(EGCSProductionCommandType::SetMode, Parameters);
	RefreshHoverMVPControls();
}

void UGCSUIBridgeSubsystem::HandleStopClicked()
{
	// Existing STOP/EStop remains a controlled LAND request. Phase 5.2B still
	// routes it through DSP-GCS/CommandManager and receives a simulated result.
	SubmitSelectedProductionCommand(EGCSProductionCommandType::Land);
}

void UGCSUIBridgeSubsystem::HandleProductionCommandResult(
	const FGCSProductionCommandResult& Result)
{
	const FString StateText =
		Result.CommandType == EGCSProductionCommandType::HoverTest && Result.bSimulated
			? TEXT("WAITING_COMMAND")
			: Result.AckState;
	LastCommandResultText = FString::Printf(
		TEXT("CMD %s  %s  %s"),
		*Result.DroneID,
		*StateText,
		*Result.CommandID);
	SetText(TEXT("Attitude"), LastCommandResultText);
	UE_LOG(
		LogTemp,
		Display,
		TEXT("DSP-GCS UI RESULT CommandID=%s Source=%s DroneID=%s Result=%s"),
		*Result.CommandID,
		*Result.Source,
		*Result.DroneID,
		*Result.AckState);
}

void UGCSUIBridgeSubsystem::SubmitSelectedProductionCommand(
	const EGCSProductionCommandType CommandType,
	const FGCSProductionCommandParameters& Parameters)
{
	UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>();
	UDSPGCSProductionCommandProviderSubsystem* Commands =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>();
	const TArray<FString> Targets = GetCommandTargetDroneIDs();
	if (!Fleet || !Commands || Targets.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("DSP-GCS UI command rejected: no checked DroneID."));
		return;
	}

	Commands->SubmitCommand(
		CommandType,
		Targets,
		Parameters);
}

void UGCSUIBridgeSubsystem::HandleFleetRowSelectionClicked(const int32 RowIndex)
{
	if (!CurrentFleetPageDroneIDs.IsValidIndex(RowIndex)
		|| CurrentFleetPageDroneIDs[RowIndex].IsEmpty())
	{
		return;
	}

	const FString& DroneID = CurrentFleetPageDroneIDs[RowIndex];
	SetCommandDroneSelected(DroneID, !SelectedCommandDroneIDs.Contains(DroneID));
	RefreshFromFleet();
}

void UGCSUIBridgeSubsystem::SetCommandDroneSelected(
	const FString& DroneID,
	const bool bSelected)
{
	if (DroneID.IsEmpty())
	{
		return;
	}

	// SelectedCommandDroneIDs 仅表示多机命令目标；这里不调用 Fleet->SelectDrone，
	// 因而不会覆盖用于详情查看/当前高亮的 CurrentSelectedDroneID。
	if (bSelected)
	{
		SelectedCommandDroneIDs.Add(DroneID);
	}
	else
	{
		SelectedCommandDroneIDs.Remove(DroneID);
	}

	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[GCS_COMMAND_SELECTION] DroneID=%s Checked=%s Targets=[%s] CurrentSelectedDroneID=%s"),
		*DroneID,
		bSelected ? TEXT("true") : TEXT("false"),
		*FString::Join(GetCommandTargetDroneIDs(), TEXT(",")),
		Fleet ? *Fleet->GetCurrentSelectedDroneID() : TEXT(""));
}

TArray<FString> UGCSUIBridgeSubsystem::GetSelectedCommandDroneIDs() const
{
	return GetCommandTargetDroneIDs();
}

FReply UGCSUIBridgeSubsystem::HandleFleetRowMouseButtonDown(
	const FGeometry& Geometry,
	const FPointerEvent& MouseEvent,
	const int32 RowIndex)
{
	(void)Geometry;
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	HandleFleetRowDetailClicked(RowIndex);
	// The row is a UI-only detail-selection hit target. Explicitly release any
	// viewport capture so a handled mouse-down can never hide the GCS cursor.
	return FReply::Handled().ReleaseMouseCapture();
}

void UGCSUIBridgeSubsystem::HandleFleetRowDetailClicked(const int32 RowIndex)
{
	if (!CurrentFleetPageDroneIDs.IsValidIndex(RowIndex)
		|| CurrentFleetPageDroneIDs[RowIndex].IsEmpty())
	{
		return;
	}

	// 点击行文本只更新详情选择；命令复选集合由独立按钮维护。
	const FString& DroneID = CurrentFleetPageDroneIDs[RowIndex];
	UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>();
	if (!Fleet || !Fleet->SelectDrone(DroneID))
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("GCS detail selection rejected: Row=%d DroneID=%s"),
			RowIndex,
			*DroneID);
	}
}

TArray<FString> UGCSUIBridgeSubsystem::GetCommandTargetDroneIDs() const
{
	TArray<FString> Result = SelectedCommandDroneIDs.Array();
	Result.Sort();
	return Result;
}

void UGCSUIBridgeSubsystem::RefreshHoverMVPControls()
{
	SetText(
		TEXT("TXT_HoverAltitudeValue"),
		FString::Printf(TEXT("%.1f m"), HoverAltitudeMeters));
	SetFleetText(
		TEXT("TXT_FleetSelectedCount"),
		FString::Printf(TEXT("SELECTED %d"), GetCommandTargetDroneIDs().Num()));

	EGCSGroundStationControlMode Mode = EGCSGroundStationControlMode::Manual;
	if (const UDSPGCSProductionCommandProviderSubsystem* Commands =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>())
	{
		Mode = Commands->GetControlMode();
	}
	SetText(
		TEXT("TXT_GroundStationMode"),
		Mode == EGCSGroundStationControlMode::Autonomous
			? TEXT("GCS MODE  AUTONOMOUS")
			: TEXT("GCS MODE  MANUAL"));
}

void UGCSUIBridgeSubsystem::SetFleetButtonLabel(
	const FName ButtonName,
	const FString& Value) const
{
	FGCSUIWidgetPresenter::SetButtonLabel(
		FleetPanelWidget.Get(),
		ButtonName,
		Value);
}

void UGCSUIBridgeSubsystem::SetLegacyHoverSelectionVisible(const bool bVisible) const
{
	UUserWidget* DroneDetail = DroneDetailWidget.Get();
	if (!DroneDetail || !DroneDetail->WidgetTree)
	{
		return;
	}

	for (const FName Name :
		{ FName(TEXT("TXT_MultiSelectHeader")),
		  FName(TEXT("BTN_SelectDrone001")),
		  FName(TEXT("BTN_SelectDrone002")),
		  FName(TEXT("BTN_SelectDrone003")),
		  FName(TEXT("TXT_SelectedDroneCount")) })
	{
		if (UWidget* Widget = DroneDetail->WidgetTree->FindWidget(Name))
		{
			Widget->SetVisibility(
				bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
	}
}

void UGCSUIBridgeSubsystem::HandleFleetChanged()
{
	RefreshFromFleet();
}

void UGCSUIBridgeSubsystem::HandleDroneDataUpdated(const FDroneData& DroneData)
{
	if (DroneData.bSelected)
	{
		UpdateDroneInfo(DroneData);
	}
}

void UGCSUIBridgeSubsystem::HandleSelectedDroneChanged(const FString& DroneID)
{
	const UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>();
	FDroneData SelectedDrone;
	if (!Fleet || !Fleet->GetDroneByID(DroneID, SelectedDrone))
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("GCS detail update rejected: selected DroneID not found: %s"),
			*DroneID);
		return;
	}

	// The selected drone affects the detail data and the Fleet row highlight.
	// Refreshing the authored widgets keeps this strictly in the presentation layer.
	RefreshFromFleet();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[GCS_SELECTION] SelectedDroneID=%s DetailUpdated Mode=%s GPS=(%.7f,%.7f) BAT=%.0f ALT=%.2f RSSI=%d"),
		*SelectedDrone.DroneID,
		*SelectedDrone.Mode,
		SelectedDrone.GPSLat,
		SelectedDrone.GPSLon,
		SelectedDrone.Battery,
		SelectedDrone.Altitude,
		SelectedDrone.RSSI);
}

void UGCSUIBridgeSubsystem::HandleFleetDataBatchApplied(
	const int64 FleetRevision,
	const int32 UpdatedDroneCount)
{
	UE_LOG(
		LogTemp,
		VeryVerbose,
		TEXT("GCS fleet batch applied: revision=%lld updated=%d"),
		FleetRevision,
		UpdatedDroneCount);
	RefreshFromFleet();
}

void UGCSUIBridgeSubsystem::HandleTelemetryLinkStateChanged()
{
	RefreshFromFleet();
	RefreshProductionCommandAvailability();
}

EDSPGCSTelemetryLinkState
UGCSUIBridgeSubsystem::GetProductionTelemetryLinkState() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UDSPGCSProductionTelemetryProviderSubsystem* Telemetry = GameInstance
		? GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>()
		: nullptr;
	// Non-Production providers and isolated UI tests retain the authoritative
	// Drone.bOnline presentation because DSP-GCS link health does not apply.
	return Telemetry && Telemetry->IsEnabled()
		? Telemetry->GetTelemetryLinkState()
		: EDSPGCSTelemetryLinkState::Connected;
}

void UGCSUIBridgeSubsystem::RefreshProductionCommandAvailability()
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UDSPGCSProductionCommandProviderSubsystem* Commands = GameInstance
		? GameInstance->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>()
		: nullptr;
	const UDSPGCSProductionTelemetryProviderSubsystem* Telemetry = GameInstance
		? GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>()
		: nullptr;
	const bool bCanSendProductionCommands = Commands && Commands->IsEnabled()
		&& Telemetry && Telemetry->IsEnabled()
		&& Telemetry->IsTelemetryLinkConnected();

	for (const FName ButtonName :
		{ FName(TEXT("BTN_Unlock")),
		  FName(TEXT("BTN_Arm")),
		  FName(TEXT("BTN_Disarm")),
		  FName(TEXT("BTN_Guided")),
		  FName(TEXT("BTN_Takeoff")),
		  FName(TEXT("BTN_RTL")),
		  FName(TEXT("BTN_Land")),
		  FName(TEXT("BTN_Hover")),
		  FName(TEXT("BTN_EStop")),
		  FName(TEXT("BTN_Stop")),
		  FName(TEXT("BTN_ModeManual")),
		  FName(TEXT("BTN_ModeAutonomous")) })
	{
		if (UButton* Button = FindButton(ButtonName))
		{
			Button->SetIsEnabled(bCanSendProductionCommands);
		}
	}
}

void UGCSUIBridgeSubsystem::RefreshFromFleet()
{
	UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>();
	if (!Fleet)
	{
		return;
	}
	RefreshProductionCommandAvailability();

	const int32 TotalCount = Fleet->GetDroneCount();
	if (TotalCount == 0)
	{
		FleetPageOffset = 0;
	}
	else if (FleetPageOffset >= TotalCount)
	{
		FleetPageOffset = ((TotalCount - 1) / FleetPageSize) * FleetPageSize;
	}

	const TArray<FDroneData> VisibleDrones =
		Fleet->GetDronePage(FleetPageOffset, FleetPageSize);
	RefreshFleetRows(
		VisibleDrones,
		TotalCount,
		Fleet->GetCurrentSelectedDroneID());

	FDroneData SelectedDrone;
	if (Fleet->GetSelectedDrone(SelectedDrone))
	{
		UpdateDroneInfo(SelectedDrone);
	}
}

void UGCSUIBridgeSubsystem::RefreshFleetRows(
	const TArray<FDroneData>& Drones,
	const int32 TotalCount,
	const FString& CurrentSelectedDroneID)
{
	const int32 PageCount = FMath::Max(1, FMath::DivideAndRoundUp(TotalCount, FleetPageSize));
	const int32 PageIndex = TotalCount == 0 ? 1 : FleetPageOffset / FleetPageSize + 1;
	SetFleetText(TEXT("FleetCount"), FString::Printf(TEXT("FLEET  %05d"), TotalCount));
	SetFleetText(TEXT("Columns"), TEXT("      ID        MODE      GPS  BAT   HGT   RSS      STATUS"));
	SetFleetText(
		TEXT("TXT_FleetPage"),
		FString::Printf(TEXT("PAGE %d / %d"), PageIndex, PageCount));
	SetFleetText(
		TEXT("TXT_FleetSelectedCount"),
		FString::Printf(TEXT("SELECTED %d"), SelectedCommandDroneIDs.Num()));

	CurrentFleetPageDroneIDs.Reset();
	CurrentFleetPageDroneIDs.SetNum(FleetPageSize);
	const EDSPGCSTelemetryLinkState ServerState =
		GetProductionTelemetryLinkState();

	for (int32 Index = 0; Index < FleetPageSize; ++Index)
	{
		const FName RowName(*FString::Printf(TEXT("Row%d"), Index + 1));
		const FName SelectorName(*FString::Printf(TEXT("BTN_FleetSelectRow%d"), Index + 1));
		const FName StatusName(*FString::Printf(TEXT("TXT_StatusRow%d"), Index + 1));
		UButton* Selector = FindFleetButton(SelectorName);
		if (!Drones.IsValidIndex(Index))
		{
			SetFleetText(RowName, FString());
			SetFleetStatus(StatusName, FString(), FLinearColor::Transparent);
			SetFleetButtonLabel(SelectorName, TEXT("[ ]"));
			UpdateFleetRowView(Index, FString(), CurrentSelectedDroneID);
			if (Selector)
			{
				Selector->SetVisibility(ESlateVisibility::Collapsed);
			}
			continue;
		}

		const FDroneData& Drone = Drones[Index];
		const EGCSDroneDisplayStatus DisplayStatus =
			ResolveDroneDisplayStatus(ServerState, Drone.bOnline);
		CurrentFleetPageDroneIDs[Index] = Drone.DroneID;
		SetFleetButtonLabel(
			SelectorName,
			SelectedCommandDroneIDs.Contains(Drone.DroneID) ? TEXT("✓") : TEXT("X"));
		if (Selector)
		{
			Selector->SetVisibility(ESlateVisibility::Visible);
		}

		const bool bHasGPS =
			!FMath::IsNearlyZero(Drone.GPSLat) || !FMath::IsNearlyZero(Drone.GPSLon);
		SetFleetText(
			RowName,
			FString::Printf(
				TEXT("      %-10s %-9s %-4s %3.0f%% %5.1f %4d"),
				*Drone.DroneID,
				*Drone.Mode,
				bHasGPS ? TEXT("OK") : TEXT("--"),
				Drone.Battery,
				Drone.Altitude,
				Drone.RSSI));
		SetFleetStatus(
			StatusName,
			FGCSUIWidgetPresenter::DisplayStatusText(DisplayStatus),
			FGCSUIWidgetPresenter::DisplayStatusColor(DisplayStatus));
		UpdateFleetRowView(Index, Drone.DroneID, CurrentSelectedDroneID);
	}
}

void UGCSUIBridgeSubsystem::InitializeFleetRowViews()
{
	FleetRowViews.Reset();
	FleetRowViews.Reserve(FleetPageSize);
	UUserWidget* FleetPanel = FleetPanelWidget.Get();
	for (int32 Index = 0; Index < FleetPageSize; ++Index)
	{
		UGCSDroneRowView* RowView = NewObject<UGCSDroneRowView>(this);
		if (!RowView->Initialize(FleetPanel, Index))
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("Fleet row %d is missing authored SelectedOverlay_Row%d Border."),
				Index + 1,
				Index + 1);
		}
		FleetRowViews.Add(RowView);
	}
}

void UGCSUIBridgeSubsystem::UpdateFleetRowView(
	const int32 RowIndex,
	const FString& DroneID,
	const FString& CurrentSelectedDroneID)
{
	if (!FleetRowViews.IsValidIndex(RowIndex) || !FleetRowViews[RowIndex])
	{
		return;
	}

	UGCSDroneRowView* RowView = FleetRowViews[RowIndex];
	RowView->SetDroneID(DroneID);
	RowView->NotifyCurrentSelectedDroneID(CurrentSelectedDroneID);
}

void UGCSUIBridgeSubsystem::UpdateDroneInfo(const FDroneData& DroneData)
{
	const EGCSDroneDisplayStatus DisplayStatus = ResolveDroneDisplayStatus(
		GetProductionTelemetryLinkState(),
		DroneData.bOnline);
	const bool bServerDisconnected =
		DisplayStatus == EGCSDroneDisplayStatus::LinkLost;
	SetTopBarText(
		TEXT("LINK"),
		bServerDisconnected
			? TEXT("SERVER DISCONNECTED")
			: (DisplayStatus == EGCSDroneDisplayStatus::Online
				? TEXT("LINK ONLINE")
				: TEXT("LINK OFFLINE")));
	SetTopBarText(TEXT("GPS"), FString::Printf(TEXT("SYS %d"), DroneData.SysID));
	SetTopBarText(TEXT("BAT"), FString::Printf(TEXT("BAT %.0f%%"), DroneData.Battery));
	SetTopBarText(TEXT("MODE"), FString::Printf(TEXT("MODE %s"), *DroneData.Mode));

	SetText(TEXT("DroneTitle"), DroneData.DroneID);
	SetText(
		TEXT("Connection"),
		bServerDisconnected
			? FString::Printf(
				TEXT("SERVER DISCONNECTED    RSSI  %d dBm"),
				DroneData.RSSI)
			: FString::Printf(
				TEXT("LINK  %s    RSSI  %d dBm"),
				DisplayStatus == EGCSDroneDisplayStatus::Online
					? TEXT("ONLINE")
					: TEXT("OFFLINE"),
				DroneData.RSSI));
	SetText(
		TEXT("Flight"),
		FString::Printf(
			TEXT("MODE  %s    %s"),
			*DroneData.Mode,
			DroneData.bSelected ? TEXT("SELECTED") : TEXT("")));
	SetText(
		TEXT("Gps"),
		FString::Printf(
			TEXT("GPS  %.7f, %.7f"),
			DroneData.GPSLat,
			DroneData.GPSLon));
	SetText(
		TEXT("Battery"),
		FString::Printf(TEXT("BAT  %.0f%%"), DroneData.Battery));
	SetText(
		TEXT("Altitude"),
		FString::Printf(TEXT("ALT  %.2f m"), DroneData.Altitude));
	SetText(
		TEXT("Speed"),
		FString::Printf(TEXT("RSSI  %d dBm"), DroneData.RSSI));
	SetText(
		TEXT("Attitude"),
		LastCommandResultText.IsEmpty()
			? TEXT("DRONE DATA")
			: LastCommandResultText);
	SetText(
		TEXT("ROLL"),
		FString::Printf(TEXT("SYSID  %d"), DroneData.SysID));
	SetText(
		TEXT("PITCH"),
		FString::Printf(
			TEXT("LED  %.2f, %.2f, %.2f"),
			DroneData.LEDColor.R,
			DroneData.LEDColor.G,
			DroneData.LEDColor.B));
	SetText(
		TEXT("YAW"),
		DroneData.bSelected ? TEXT("SELECTED  YES") : TEXT("SELECTED  NO"));
}

UButton* UGCSUIBridgeSubsystem::FindButton(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindButton(DroneDetailWidget.Get(), Name);
}

UButton* UGCSUIBridgeSubsystem::FindFleetButton(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindButton(FleetPanelWidget.Get(), Name);
}

UButton* UGCSUIBridgeSubsystem::FindToolbarButton(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindButton(LeftToolbarWidget.Get(), Name);
}

UTextBlock* UGCSUIBridgeSubsystem::FindText(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindText(DroneDetailWidget.Get(), Name);
}

UTextBlock* UGCSUIBridgeSubsystem::FindFleetText(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindText(FleetPanelWidget.Get(), Name);
}

UTextBlock* UGCSUIBridgeSubsystem::FindTopBarText(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindText(TopBarWidget.Get(), Name);
}

void UGCSUIBridgeSubsystem::SetText(
	const FName Name,
	const FString& Value) const
{
	FGCSUIWidgetPresenter::SetText(DroneDetailWidget.Get(), Name, Value);
}

void UGCSUIBridgeSubsystem::SetFleetText(
	const FName Name,
	const FString& Value) const
{
	FGCSUIWidgetPresenter::SetText(FleetPanelWidget.Get(), Name, Value);
}

void UGCSUIBridgeSubsystem::SetFleetStatus(
	const FName Name,
	const FString& Value,
	const FLinearColor& Color) const
{
	FGCSUIWidgetPresenter::SetStatus(
		FleetPanelWidget.Get(),
		Name,
		Value,
		Color);
}

void UGCSUIBridgeSubsystem::SetTopBarText(
	const FName Name,
	const FString& Value) const
{
	FGCSUIWidgetPresenter::SetText(TopBarWidget.Get(), Name, Value);
}

void UGCSUIBridgeSubsystem::BindExistingButtons()
{
	if (UButton* Button = FindButton(TEXT("BTN_Unlock")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleArmClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleArmClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_Arm")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleArmClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleArmClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_Disarm")))
	{
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleDisarmClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_Guided")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleGuidedClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleGuidedClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_Takeoff")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleTakeoffClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleTakeoffClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_RTL")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleRTLClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleRTLClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_Land")))
	{
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleLandClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_Hover")))
	{
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleHoverClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_EStop")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleStopClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleStopClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_Stop")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleStopClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleStopClicked);
	}

	if (UButton* Button = FindButton(TEXT("BTN_HoverAltitudeMinus")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverAltitudeMinusClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverAltitudeMinusClicked);
	}
	if (UButton* Button = FindButton(TEXT("BTN_HoverAltitudePlus")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverAltitudePlusClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverAltitudePlusClicked);
	}
	if (UButton* Button = FindButton(TEXT("BTN_ModeManual")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleManualModeClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleManualModeClicked);
	}
	if (UButton* Button = FindButton(TEXT("BTN_ModeAutonomous")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleAutonomousModeClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleAutonomousModeClicked);
	}
}

void UGCSUIBridgeSubsystem::BindFleetPanelButtons()
{
	InitializeFleetRowViews();

	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetSelectRow1")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow1SelectionClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow1SelectionClicked);
	}
	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetSelectRow2")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow2SelectionClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow2SelectionClicked);
	}
	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetSelectRow3")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow3SelectionClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow3SelectionClicked);
	}
	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetSelectRow4")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow4SelectionClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow4SelectionClicked);
	}
	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetSelectRow5")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow5SelectionClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetRow5SelectionClicked);
	}

	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetPreviousPage")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetPreviousPageClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetPreviousPageClicked);
	}
	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetNextPage")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetNextPageClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetNextPageClicked);
	}

	// The authored FleetPanel has separate checkbox buttons and row text.
	// Reuse the row text hit area for the single detail selection without
	// adding or rearranging any UMG widgets.
	for (int32 Index = 0; Index < FleetPageSize; ++Index)
	{
		const FName RowName(*FString::Printf(TEXT("Row%d"), Index + 1));
		if (UTextBlock* Row = FindFleetText(RowName))
		{
			Row->SetVisibility(ESlateVisibility::Visible);
			Row->TakeWidget()->SetOnMouseButtonDown(
				FPointerEventHandler::CreateUObject(
					this,
					&UGCSUIBridgeSubsystem::HandleFleetRowMouseButtonDown,
					Index));
		}
	}
}

void UGCSUIBridgeSubsystem::BindNavigationButtons()
{
	if (UButton* Button = FindToolbarButton(TEXT("BTN_Menu")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleMenuClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleMenuClicked);
	}

	if (UButton* Button = FindToolbarButton(TEXT("BTN_Fleet")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetPageClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleFleetPageClicked);
	}

	if (UButton* Button = FindToolbarButton(TEXT("BTN_Control")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleControlPageClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleControlPageClicked);
	}

	if (UButton* Button = FindToolbarButton(TEXT("BTN_Map")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleMapPageClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleMapPageClicked);
	}

	if (UButton* Button = FindToolbarButton(TEXT("BTN_Settings")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleSettingsPageClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleSettingsPageClicked);
	}

	if (UButton* Button = FindToolbarButton(TEXT("BTN_Logs")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleLogsPageClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleLogsPageClicked);
	}
}

void UGCSUIBridgeSubsystem::CreateNavigationPages()
{
	MapPageWidget = CreateNavigationPage(
		TEXT("/Game/UI/WBP_GCS_PageMap.WBP_GCS_PageMap_C"));
	SettingsPageWidget = CreateNavigationPage(
		TEXT("/Game/UI/WBP_GCS_PageSettings.WBP_GCS_PageSettings_C"));
	LogsPageWidget = CreateNavigationPage(
		TEXT("/Game/UI/WBP_GCS_PageLogs.WBP_GCS_PageLogs_C"));
}

void UGCSUIBridgeSubsystem::ResetNavigationPages()
{
	for (TWeakObjectPtr<UUserWidget>* Page :
		{ &MapPageWidget, &SettingsPageWidget, &LogsPageWidget })
	{
		if (UUserWidget* Widget = Page->Get())
		{
			Widget->RemoveFromParent();
		}

		Page->Reset();
	}
}

void UGCSUIBridgeSubsystem::ShowMainPage(const EGCSMainPage Page)
{
	ActivePage = Page;
	bPanelsExpanded = true;
	ApplyNavigationVisibility();
}

void UGCSUIBridgeSubsystem::ApplyNavigationVisibility()
{
	const bool bShowFleet =
		bPanelsExpanded && ActivePage == EGCSMainPage::Fleet;
	const bool bShowDroneDetail =
		bPanelsExpanded
		&& (ActivePage == EGCSMainPage::Fleet
			|| ActivePage == EGCSMainPage::Control);

	FGCSUIWidgetPresenter::SetWidgetVisible(FleetPanelWidget, bShowFleet);
	FGCSUIWidgetPresenter::SetWidgetVisible(DroneDetailWidget, bShowDroneDetail);
	FGCSUIWidgetPresenter::SetWidgetVisible(
		MapPageWidget,
		ActivePage == EGCSMainPage::Map,
		true);
	FGCSUIWidgetPresenter::SetWidgetVisible(
		SettingsPageWidget,
		ActivePage == EGCSMainPage::Settings,
		true);
	FGCSUIWidgetPresenter::SetWidgetVisible(
		LogsPageWidget,
		ActivePage == EGCSMainPage::Logs,
		true);

	UE_LOG(LogTemp, Log, TEXT("GCS UI navigation changed to page %d"), static_cast<int32>(ActivePage));
}

UUserWidget* UGCSUIBridgeSubsystem::CreateNavigationPage(
	const TCHAR* ClassPath) const
{
	UUserWidget* Owner = MainWidgetRef.Get();
	APlayerController* PlayerController = Owner ? Owner->GetOwningPlayer() : nullptr;
	const TSubclassOf<UUserWidget> PageClass = LoadClass<UUserWidget>(nullptr, ClassPath);

	if (!PlayerController || !PageClass)
	{
		UE_LOG(LogTemp, Error, TEXT("Unable to load GCS navigation page: %s"), ClassPath);
		return nullptr;
	}

	UUserWidget* Page = CreateWidget<UUserWidget>(PlayerController, PageClass);

	if (Page)
	{
		// Visual content remains authored in the Widget Blueprint asset. C++ only
		// owns page lifetime and navigation state.
		Page->AddToViewport(95);
		Page->SetAnchorsInViewport(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		Page->SetAlignmentInViewport(FVector2D::ZeroVector);
		Page->SetVisibility(ESlateVisibility::Collapsed);
	}

	return Page;
}
