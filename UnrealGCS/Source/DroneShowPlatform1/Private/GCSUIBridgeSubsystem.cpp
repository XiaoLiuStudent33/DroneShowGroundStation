/**
 * @brief 实现主 UI 绑定、Fleet 分页、详情显示、命令选择和页面导航。
 *
 * 分类：Production。
 * 当前命令只调用 DSPGCSProductionCommandProviderSubsystem。
 */
#include "GCSUIBridgeSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "DSPGCSProductionCommandProviderSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "DroneFleetVisualizationActor.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"
#include "FleetManagerSubsystem.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GCSDroneRowView.h"
#include "GCSUIWidgetPresenter.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Math/RotationMatrix.h"
#include "RoutePreviewSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWidget.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShObjIdl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	bool TryParseFiniteDouble(const FText& Text, double& OutValue)
	{
		const FString ValueString = Text.ToString().TrimStartAndEnd();
		return !ValueString.IsEmpty()
			&& LexTryParseString(OutValue, *ValueString)
			&& FMath::IsFinite(OutValue);
	}

	FString FormatRouteDuration(const double Seconds)
	{
		const int32 TotalSeconds = FMath::Max(0, FMath::FloorToInt(Seconds));
		return FString::Printf(TEXT("%02d:%02d"), TotalSeconds / 60, TotalSeconds % 60);
	}

	bool OpenRuntimeNpzFileDialog(FString& OutFilePath)
	{
		OutFilePath.Reset();
#if PLATFORM_WINDOWS
		const HRESULT InitializeResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const bool bUninitializeCom = SUCCEEDED(InitializeResult);
		if (FAILED(InitializeResult) && InitializeResult != RPC_E_CHANGED_MODE)
		{
			return false;
		}

		IFileOpenDialog* Dialog = nullptr;
		HRESULT Result = ::CoCreateInstance(
			CLSID_FileOpenDialog,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&Dialog));
		if (SUCCEEDED(Result) && Dialog)
		{
			const COMDLG_FILTERSPEC Filters[] =
			{
				{ L"NumPy archives (*.npz)", L"*.npz" }
			};
			Dialog->SetFileTypes(UE_ARRAY_COUNT(Filters), Filters);
			Dialog->SetFileTypeIndex(1);
			Dialog->SetDefaultExtension(L"npz");

			FILEOPENDIALOGOPTIONS Options = {};
			if (SUCCEEDED(Dialog->GetOptions(&Options)))
			{
				Dialog->SetOptions(Options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
			}

			const HWND ParentWindow = FSlateApplication::IsInitialized()
				? static_cast<HWND>(const_cast<void*>(
					FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)))
				: nullptr;
			Result = Dialog->Show(ParentWindow);
			if (SUCCEEDED(Result))
			{
				IShellItem* SelectedItem = nullptr;
				if (SUCCEEDED(Dialog->GetResult(&SelectedItem)) && SelectedItem)
				{
					PWSTR SelectedPath = nullptr;
					if (SUCCEEDED(SelectedItem->GetDisplayName(SIGDN_FILESYSPATH, &SelectedPath))
						&& SelectedPath)
					{
						OutFilePath = SelectedPath;
						::CoTaskMemFree(SelectedPath);
					}
					SelectedItem->Release();
				}
			}
			Dialog->Release();
		}

		if (bUninitializeCom)
		{
			::CoUninitialize();
		}
		return !OutFilePath.IsEmpty();
#else
		return false;
#endif
	}
}

class FGCSMarqueeInputProcessor final : public IInputProcessor
{
public:
	explicit FGCSMarqueeInputProcessor(UGCSUIBridgeSubsystem* InOwner)
		: Owner(InOwner)
	{
	}

	virtual void Tick(
		const float DeltaTime,
		FSlateApplication& SlateApp,
		TSharedRef<ICursor> Cursor) override
	{
		(void)DeltaTime;
		(void)SlateApp;
		(void)Cursor;
		if (UGCSUIBridgeSubsystem* Bridge = Owner.Get())
		{
			Bridge->PollMarqueePlayerControllerInput();
			Bridge->TickRoutePlaybackUI(DeltaTime);
		}
	}

private:
	TWeakObjectPtr<UGCSUIBridgeSubsystem> Owner;
};

UGCSUIBridgeSubsystem::UGCSUIBridgeSubsystem()
{
	static ConstructorHelpers::FClassFinder<UUserWidget> RouteImportClassFinder(
		TEXT("/Game/UI/WBP_GCS_RouteImport"));
	RouteImportWidgetClass = RouteImportClassFinder.Class;
}

void UGCSUIBridgeSubsystem::BindMainWidget(UUserWidget* MainWidget)
{
	UnregisterMarqueeInputProcessor();
	ResetNavigationPages();

	MainWidgetRef = MainWidget;
	TopBarWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_TopBar"));
	LeftToolbarWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_LeftToolbar"));
	FleetPanelWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_FleetPanel"));
	RightToolbarWidget =
		FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_RightToolbar"));
	RightPanelWidget =
		FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_RightPanel"));
	PlaybackWidget =
		FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_Playback"));
	SelectionRectangleWidget = MainWidget && MainWidget->WidgetTree
		? MainWidget->WidgetTree->FindWidget(TEXT("SelectionRectangle"))
		: nullptr;
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
	FGCSUIWidgetPresenter::SetWidgetVisible(FleetPanelWidget, false);
	FGCSUIWidgetPresenter::SetWidgetVisible(DroneDetailWidget, false);
	FGCSUIWidgetPresenter::SetWidgetVisible(RightPanelWidget, false);
	UpdateSelectionRectangleVisual(FVector2D::ZeroVector, FVector2D::ZeroVector, false);

	if (UButton* ViewButton =
		FGCSUIWidgetPresenter::FindButton(RightToolbarWidget.Get(), TEXT("BTN_View")))
	{
		const TWeakObjectPtr<UUserWidget> RightPanel = RightPanelWidget;
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

	UButton* LocateButton =
		FGCSUIWidgetPresenter::FindButton(RightToolbarWidget.Get(), TEXT("BTN_ZCamera"));
	if (!LocateButton)
	{
		// Keep compatibility with a toolbar authored before the production rename.
		LocateButton = FGCSUIWidgetPresenter::FindButton(RightToolbarWidget.Get(), TEXT("BTN_GPS"));
	}
	if (LocateButton)
	{
		LocateButton->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleTopDownLocateClicked);
		LocateButton->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleTopDownLocateClicked);
		UE_LOG(LogTemp, Log, TEXT("[GCSLocate] BTN_ZCamera Bound = YES"));
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
	BindColorPaletteButtons();
	BindRoutePlaybackControls();
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

	if (SelectionRectangleWidget.IsValid())
	{
		RegisterMarqueeInputProcessor();
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("WBP_GCS_Main_V2 is missing authored SelectionRectangle."));
	}
}

void UGCSUIBridgeSubsystem::Deinitialize()
{
	UnregisterMarqueeInputProcessor();
	EndTopDownLocateLock();
	if (UUserWidget* ImportWidget = RouteImportWidget.Get())
	{
		ImportWidget->RemoveFromParent();
	}
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
	RightToolbarWidget.Reset();
	RightPanelWidget.Reset();
	PlaybackWidget.Reset();
	RouteImportWidget.Reset();
	RouteImportButton.Reset();
	RouteImportCloseButton.Reset();
	RouteBrowseButton.Reset();
	RouteCancelButton.Reset();
	RouteLoadPreviewButton.Reset();
	RouteSampleRateInput.Reset();
	RouteOriginLatitudeInput.Reset();
	RouteOriginLongitudeInput.Reset();
	RouteOriginAltitudeInput.Reset();
	RouteImportFilePathText.Reset();
	RouteImportDroneValueText.Reset();
	RouteImportFrameCountValueText.Reset();
	RouteImportDurationValueText.Reset();
	RoutePreviousButton.Reset();
	RoutePauseButton.Reset();
	RoutePlayButton.Reset();
	RouteStopButton.Reset();
	RouteNextButton.Reset();
	RouteExitPreviewButton.Reset();
	RouteUnloadButton.Reset();
	RouteTimelineSlider.Reset();
	RouteTimeText.Reset();
	RouteFrameText.Reset();
	SelectionRectangleWidget.Reset();
	LastCommandResultText.Reset();
	SelectedRouteImportFilePath.Reset();
	SelectedCommandDroneIDs.Reset();
	CurrentFleetPageDroneIDs.Reset();
	FleetRowViews.Reset();
	FleetPageOffset = 0;
	bMarqueePointerDown = false;
	bMarqueeSelecting = false;
	bMarqueeLeftMouseWasDown = false;
	MarqueeCurrentLocalPosition = FVector2D::ZeroVector;
	bUpdatingRouteTimelineFromState = false;
	RouteUIRefreshAccumulator = 0.0f;

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
	if (UUserWidget* FleetPanel = FleetPanelWidget.Get())
	{
		const ESlateVisibility CurrentVisibility = FleetPanel->GetVisibility();
		const bool bCurrentlyVisible =
			CurrentVisibility != ESlateVisibility::Collapsed
			&& CurrentVisibility != ESlateVisibility::Hidden;
		FleetPanel->SetVisibility(
			bCurrentlyVisible
				? ESlateVisibility::Collapsed
				: ESlateVisibility::Visible);
	}
}

void UGCSUIBridgeSubsystem::HandleControlPageClicked()
{
	if (UUserWidget* DroneDetail = DroneDetailWidget.Get())
	{
		const ESlateVisibility CurrentVisibility = DroneDetail->GetVisibility();
		const bool bCurrentlyVisible =
			CurrentVisibility != ESlateVisibility::Collapsed
			&& CurrentVisibility != ESlateVisibility::Hidden;
		DroneDetail->SetVisibility(
			bCurrentlyVisible
				? ESlateVisibility::Collapsed
				: ESlateVisibility::Visible);
	}
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
	if (SelectedCommandDroneIDs.Contains(DroneID) == bSelected)
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
	SyncCommandSelectionToVisualization();

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

void UGCSUIBridgeSubsystem::ReplaceCommandDroneSelection(
	const TArray<FString>& DroneIDs)
{
	SelectedCommandDroneIDs.Reset();
	for (const FString& DroneID : DroneIDs)
	{
		if (!DroneID.IsEmpty())
		{
			SelectedCommandDroneIDs.Add(DroneID);
		}
	}
	SyncCommandSelectionToVisualization();
	RefreshFromFleet();
}

void UGCSUIBridgeSubsystem::RegisterMarqueeInputProcessor()
{
	if (MarqueeInputProcessor.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}

	MarqueeInputProcessor = MakeShared<FGCSMarqueeInputProcessor>(this);
	FSlateApplication::Get().RegisterInputPreProcessor(MarqueeInputProcessor, 0);
}

void UGCSUIBridgeSubsystem::UnregisterMarqueeInputProcessor()
{
	if (MarqueeInputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(MarqueeInputProcessor);
	}
	MarqueeInputProcessor.Reset();
	EndMarqueeCameraLock();
	EndTopDownLocateLock();
	UpdateSelectionRectangleVisual(FVector2D::ZeroVector, FVector2D::ZeroVector, false);
	bMarqueePointerDown = false;
	bMarqueeSelecting = false;
	bMarqueeLeftMouseWasDown = false;
}

void UGCSUIBridgeSubsystem::PollMarqueePlayerControllerInput()
{
	UUserWidget* MainWidget = MainWidgetRef.Get();
	APlayerController* PlayerController = MainWidget ? MainWidget->GetOwningPlayer() : nullptr;
	UWorld* World = MainWidget ? MainWidget->GetWorld() : nullptr;
	if (!MainWidget || !PlayerController || !World)
	{
		EndMarqueeCameraLock();
		EndTopDownLocateLock();
		UpdateSelectionRectangleVisual(FVector2D::ZeroVector, FVector2D::ZeroVector, false);
		bMarqueePointerDown = false;
		bMarqueeSelecting = false;
		bMarqueeLeftMouseWasDown = false;
		return;
	}
	if (!FApp::HasFocus())
	{
		EndMarqueeCameraLock();
		UpdateSelectionRectangleVisual(FVector2D::ZeroVector, FVector2D::ZeroVector, false);
		bMarqueePointerDown = false;
		bMarqueeSelecting = false;
		bMarqueeLeftMouseWasDown = false;
		return;
	}

	const bool bLeftMouseDown = PlayerController->IsInputKeyDown(EKeys::LeftMouseButton);
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	const bool bMousePositionValid = PlayerController->GetMousePosition(MouseX, MouseY);
	const FVector2D ScreenPosition = FSlateApplication::Get().GetCursorPos();
	const FVector2D LocalPosition = bMousePositionValid
		? UWidgetLayoutLibrary::GetMousePositionOnViewport(World)
		: MarqueeCurrentLocalPosition;

	UpdateTopDownViewInput(PlayerController);
	UpdateTopDownLocateLock(PlayerController);

	if (bLeftMouseDown && !bMarqueeLeftMouseWasDown)
	{
		bMarqueeLeftMouseWasDown = true;
		const bool bBlockedByUI = IsPointerOverBlockingGCSUI(ScreenPosition);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[GCS_MARQUEE_INPUT] MouseDown PositionValid=%s BlockedByUI=%s"),
			bMousePositionValid ? TEXT("true") : TEXT("false"),
			bBlockedByUI ? TEXT("true") : TEXT("false"));
		if (!bMousePositionValid || bBlockedByUI)
		{
			return;
		}

		bMarqueePointerDown = true;
		bMarqueeSelecting = false;
		MarqueeStartScreenPosition = ScreenPosition;
		MarqueeStartLocalPosition = LocalPosition;
		MarqueeCurrentLocalPosition = LocalPosition;
		UpdateSelectionRectangleVisual(LocalPosition, LocalPosition, false);
		return;
	}

	if (bLeftMouseDown && bMarqueePointerDown && bMousePositionValid)
	{
		MarqueeCurrentLocalPosition = LocalPosition;
		if (!bMarqueeSelecting
			&& FVector2D::Distance(ScreenPosition, MarqueeStartScreenPosition)
				>= MarqueeDragThresholdPixels)
		{
			bMarqueeSelecting = true;
			BeginMarqueeCameraLock(PlayerController);
			UE_LOG(LogTemp, Display, TEXT("[GCS_MARQUEE_INPUT] DragStart"));
		}

		if (bMarqueeSelecting)
		{
			UpdateSelectionRectangleVisual(
				MarqueeStartLocalPosition,
				MarqueeCurrentLocalPosition,
				true);
		}
		return;
	}

	if (!bLeftMouseDown && bMarqueeLeftMouseWasDown)
	{
		bMarqueeLeftMouseWasDown = false;
		if (!bMarqueePointerDown)
		{
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("[GCS_MARQUEE_INPUT] MouseUp"));
		if (bMarqueeSelecting)
		{
			CompleteMarqueeSelection(
				MarqueeStartLocalPosition,
				MarqueeCurrentLocalPosition);
		}
		UpdateSelectionRectangleVisual(FVector2D::ZeroVector, FVector2D::ZeroVector, false);
		bMarqueePointerDown = false;
		bMarqueeSelecting = false;
		EndMarqueeCameraLock();
	}
}

void UGCSUIBridgeSubsystem::BeginMarqueeCameraLock(APlayerController* PlayerController)
{
	if (bMarqueeCameraLocked || !PlayerController)
	{
		return;
	}

	MarqueeCameraLockPlayerController = PlayerController;
	PlayerController->SetIgnoreLookInput(true);
	bMarqueeCameraLocked = true;
}

void UGCSUIBridgeSubsystem::EndMarqueeCameraLock()
{
	if (!bMarqueeCameraLocked)
	{
		MarqueeCameraLockPlayerController.Reset();
		return;
	}

	if (APlayerController* PlayerController = MarqueeCameraLockPlayerController.Get())
	{
		PlayerController->SetIgnoreLookInput(false);
	}
	MarqueeCameraLockPlayerController.Reset();
	bMarqueeCameraLocked = false;
}

void UGCSUIBridgeSubsystem::UpdateTopDownViewInput(APlayerController* PlayerController)
{
	if (!bTopDownLocateLocked || !PlayerController)
	{
		ResetTopDownViewInputState();
		return;
	}

	ACameraActor* TopDownCamera = TopDownOrthographicCamera.Get();
	UCameraComponent* CameraComponent = TopDownCamera
		? TopDownCamera->GetCameraComponent()
		: nullptr;
	if (!TopDownCamera || !CameraComponent || TopDownFitOrthoWidth <= KINDA_SMALL_NUMBER)
	{
		ResetTopDownViewInputState();
		return;
	}

	const float MouseWheelDelta = PlayerController->GetInputAnalogKeyState(EKeys::MouseWheelAxis);
	if (!FMath::IsNearlyZero(MouseWheelDelta))
	{
		const float MinOrthoWidth = FMath::Max(
			KINDA_SMALL_NUMBER,
			TopDownFitOrthoWidth * TopDownMinOrthoWidthFactor);
		const float MaxOrthoWidth = FMath::Max(
			TopDownFitOrthoWidth,
			TopDownFitOrthoWidth * TopDownMaxOrthoWidthFactor);
		const float ZoomScale = FMath::Pow(TopDownZoomFactor, MouseWheelDelta);
		CameraComponent->OrthoWidth = FMath::Clamp(
			CameraComponent->OrthoWidth * ZoomScale,
			MinOrthoWidth,
			MaxOrthoWidth);
	}

	const bool bMiddleMouseDown = PlayerController->IsInputKeyDown(EKeys::MiddleMouseButton);
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	const bool bMousePositionValid = PlayerController->GetMousePosition(MouseX, MouseY);
	const FVector2D CurrentMousePosition(MouseX, MouseY);

	if (bMiddleMouseDown && !bTopDownMiddleMouseWasDown)
	{
		bTopDownMiddleMouseWasDown = true;
		bTopDownPanning = bMousePositionValid
			&& !PlayerController->IsInputKeyDown(EKeys::LeftMouseButton)
			&& !bMarqueePointerDown
			&& !bMarqueeSelecting;
		TopDownLastMousePosition = CurrentMousePosition;
	}

	if (bTopDownPanning
		&& (PlayerController->IsInputKeyDown(EKeys::LeftMouseButton)
			|| bMarqueePointerDown
			|| bMarqueeSelecting))
	{
		bTopDownPanning = false;
	}

	if (bMiddleMouseDown && bTopDownPanning && bMousePositionValid)
	{
		const FVector2D MouseDelta = CurrentMousePosition - TopDownLastMousePosition;
		int32 ViewportWidth = 0;
		int32 ViewportHeight = 0;
		PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
		(void)ViewportHeight;
		if (ViewportWidth > 0 && !MouseDelta.IsNearlyZero())
		{
			FVector CameraRight = bTopDownGeographicBasisActive
				? TopDownLocalEast
				: TopDownCamera->GetActorRightVector();
			FVector CameraUp = bTopDownGeographicBasisActive
				? TopDownLocalNorth
				: TopDownCamera->GetActorUpVector();
			if (!bTopDownGeographicBasisActive)
			{
				CameraRight.Z = 0.0f;
				CameraUp.Z = 0.0f;
				CameraRight = CameraRight.GetSafeNormal();
				CameraUp = CameraUp.GetSafeNormal();
			}
			if (!CameraRight.IsNearlyZero() && !CameraUp.IsNearlyZero())
			{
				const float WorldUnitsPerPixel =
					CameraComponent->OrthoWidth / static_cast<float>(ViewportWidth);
				const FVector CameraMove =
					(-MouseDelta.X * CameraRight + MouseDelta.Y * CameraUp)
					* WorldUnitsPerPixel;
				TopDownLockedCameraLocation += CameraMove;
			}
		}
		TopDownLastMousePosition = CurrentMousePosition;
	}

	if (!bMiddleMouseDown && bTopDownMiddleMouseWasDown)
	{
		ResetTopDownViewInputState();
	}
}

void UGCSUIBridgeSubsystem::ResetTopDownViewInputState()
{
	bTopDownMiddleMouseWasDown = false;
	bTopDownPanning = false;
	TopDownLastMousePosition = FVector2D::ZeroVector;
}

void UGCSUIBridgeSubsystem::HandleTopDownLocateClicked()
{
	UE_LOG(LogTemp, Log, TEXT("[GCSLocate] TopDown Locate Clicked"));
	UUserWidget* MainWidget = MainWidgetRef.Get();
	UWorld* World = MainWidget ? MainWidget->GetWorld() : nullptr;
	APlayerController* PlayerController = MainWidget ? MainWidget->GetOwningPlayer() : nullptr;
	if (!World || !PlayerController)
	{
		return;
	}
	if (bTopDownLocateLocked)
	{
		EndTopDownLocateLock(PlayerController);
		UE_LOG(LogTemp, Log, TEXT("[GCSLocate] TopDown Locate Exited By Toggle"));
		return;
	}

	ADroneFleetVisualizationActor* Visualization = nullptr;
	TArray<FVector> WorldPositions;
	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		Visualization = *It;
		WorldPositions = It->GetDronePositions();
		break;
	}
	if (!Visualization || WorldPositions.IsEmpty())
	{
		return;
	}

	FVector BasisOriginWorld = FVector::ZeroVector;
	bTopDownGeographicBasisActive = Visualization->TryGetGeographicTangentBasis(
		BasisOriginWorld,
		TopDownLocalEast,
		TopDownLocalNorth,
		TopDownLocalUp);

	FBox2D FleetPlaneBounds(EForceInit::ForceInit);
	double FleetMaxUp = -TNumericLimits<double>::Max();
	for (const FVector& WorldPosition : WorldPositions)
	{
		if (WorldPosition.ContainsNaN())
		{
			continue;
		}

		if (bTopDownGeographicBasisActive)
		{
			const FVector RelativePosition = WorldPosition - BasisOriginWorld;
			const double East = FVector::DotProduct(RelativePosition, TopDownLocalEast);
			const double North = FVector::DotProduct(RelativePosition, TopDownLocalNorth);
			const double Up = FVector::DotProduct(RelativePosition, TopDownLocalUp);
			FleetPlaneBounds += FVector2D(East, North);
			FleetMaxUp = FMath::Max(FleetMaxUp, Up);
		}
		else
		{
			FleetPlaneBounds += FVector2D(WorldPosition.X, WorldPosition.Y);
			FleetMaxUp = FMath::Max(FleetMaxUp, static_cast<double>(WorldPosition.Z));
		}
	}
	if (!FleetPlaneBounds.bIsValid || FleetMaxUp == -TNumericLimits<double>::Max())
	{
		return;
	}

	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
	const float AspectRatio = ViewportHeight > 0
		? static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight)
		: (16.0f / 9.0f);
	const FVector2D FleetCenterPlane = FleetPlaneBounds.GetCenter();
	const FVector2D FleetSizePlane = FleetPlaneBounds.GetSize();
	const float OrthoWidth = FMath::Max(
		1000.0f,
		FMath::Max(
			FleetSizePlane.X,
			FleetSizePlane.Y * FMath::Max(AspectRatio, KINDA_SMALL_NUMBER)) * 1.15f);

	if (bTopDownGeographicBasisActive)
	{
		const FVector FleetCenterWorld =
			BasisOriginWorld
			+ TopDownLocalEast * FleetCenterPlane.X
			+ TopDownLocalNorth * FleetCenterPlane.Y;
		TopDownLockedCameraLocation =
			FleetCenterWorld + TopDownLocalUp * (FleetMaxUp + 1000.0);
		TopDownLockedCameraRotation =
			FRotationMatrix::MakeFromXY(-TopDownLocalUp, TopDownLocalEast).Rotator();
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[GCSLocate] TopDown geographic basis active Origin=%s East=%s North=%s Up=%s OrthoWidth=%.3f"),
			*BasisOriginWorld.ToString(),
			*TopDownLocalEast.ToString(),
			*TopDownLocalNorth.ToString(),
			*TopDownLocalUp.ToString(),
			OrthoWidth);
	}
	else
	{
		TopDownLocalEast = FVector::XAxisVector;
		TopDownLocalNorth = -FVector::YAxisVector;
		TopDownLocalUp = FVector::ZAxisVector;
		TopDownLockedCameraLocation = FVector(
			FleetCenterPlane.X,
			FleetCenterPlane.Y,
			FleetMaxUp + 1000.0);
		TopDownLockedCameraRotation = FRotator(-90.0f, 0.0f, 0.0f);
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[GCSLocate] TopDown geographic basis unavailable, falling back to world topdown"));
	}

	if (!TopDownOrthographicCamera.IsValid())
	{
		TopDownOrthographicCamera = World->SpawnActor<ACameraActor>();
		if (!TopDownOrthographicCamera.IsValid())
		{
			return;
		}
		TopDownOrthographicCamera->SetActorEnableCollision(false);
	}

	ACameraActor* TopDownCamera = TopDownOrthographicCamera.Get();
	UCameraComponent* CameraComponent = TopDownCamera->GetCameraComponent();
	if (!CameraComponent)
	{
		return;
	}
	CameraComponent->ProjectionMode = ECameraProjectionMode::Orthographic;
	CameraComponent->OrthoWidth = OrthoWidth;
	TopDownFitOrthoWidth = OrthoWidth;
	ResetTopDownViewInputState();
	TopDownCamera->SetActorLocationAndRotation(
		TopDownLockedCameraLocation,
		TopDownLockedCameraRotation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	if (!bTopDownLocateLocked)
	{
		PreviousTopDownViewTarget = PlayerController->GetViewTarget();
		TopDownPlayerController = PlayerController;
		PreviousTopDownControlRotation = PlayerController->GetControlRotation();
		bHasPreviousTopDownControlRotation = true;
		UE_LOG(
			LogTemp,
			Log,
			TEXT("[GCSLocate] TopDown enter: Saved ViewTarget=%s Saved ControlRotation=%s"),
			PreviousTopDownViewTarget.IsValid()
				? *PreviousTopDownViewTarget->GetName()
				: TEXT("None"),
			*PreviousTopDownControlRotation.ToString());
	}
	PlayerController->SetControlRotation(TopDownLockedCameraRotation);
	PlayerController->SetViewTargetWithBlend(TopDownCamera, 0.2f);
	bTopDownLocateLocked = true;
	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		It->SetDroneLabelsVisible(true);
		break;
	}
}

void UGCSUIBridgeSubsystem::HandleColorOffClicked()
{
	ApplyAndSubmitSelectedDroneLedColor(FLinearColor::Black, 0, 0, 0);
}

void UGCSUIBridgeSubsystem::HandleColorWhiteClicked()
{
	ApplyAndSubmitSelectedDroneLedColor(FLinearColor::White, 255, 255, 255);
}

void UGCSUIBridgeSubsystem::HandleColorRedClicked()
{
	ApplyAndSubmitSelectedDroneLedColor(
		FLinearColor::FromSRGBColor(FColor(255, 48, 48)),
		255,
		48,
		48);
}

void UGCSUIBridgeSubsystem::HandleColorGreenClicked()
{
	ApplyAndSubmitSelectedDroneLedColor(
		FLinearColor::FromSRGBColor(FColor(53, 224, 91)),
		53,
		224,
		91);
}

void UGCSUIBridgeSubsystem::HandleColorBlueClicked()
{
	ApplyAndSubmitSelectedDroneLedColor(
		FLinearColor::FromSRGBColor(FColor(50, 130, 255)),
		50,
		130,
		255);
}

void UGCSUIBridgeSubsystem::HandleColorYellowClicked()
{
	ApplyAndSubmitSelectedDroneLedColor(
		FLinearColor::FromSRGBColor(FColor(255, 211, 56)),
		255,
		211,
		56);
}

void UGCSUIBridgeSubsystem::HandleColorMagentaClicked()
{
	ApplyAndSubmitSelectedDroneLedColor(
		FLinearColor::FromSRGBColor(FColor(197, 74, 255)),
		197,
		74,
		255);
}

void UGCSUIBridgeSubsystem::HandleRoutePreviousFrameClicked()
{
	StepRoutePreviewFrame(-1);
}

void UGCSUIBridgeSubsystem::HandleRoutePauseClicked()
{
	if (URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>())
	{
		RoutePreview->Pause();
		RefreshRoutePlaybackUI();
	}
}

void UGCSUIBridgeSubsystem::HandleRoutePlayClicked()
{
	if (URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>())
	{
		FString Error;
		if (!RoutePreview->Play(Error))
		{
			UE_LOG(LogTemp, Warning, TEXT("[RoutePreviewUI] Play rejected: %s"), *Error);
		}
		RefreshRoutePlaybackUI();
	}
}

void UGCSUIBridgeSubsystem::HandleRouteStopClicked()
{
	if (URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>())
	{
		FString Error;
		if (!RoutePreview->Stop(Error))
		{
			UE_LOG(LogTemp, Warning, TEXT("[RoutePreviewUI] Stop rejected: %s"), *Error);
		}
		RefreshRoutePlaybackUI();
	}
}

void UGCSUIBridgeSubsystem::HandleRouteNextFrameClicked()
{
	StepRoutePreviewFrame(1);
}

void UGCSUIBridgeSubsystem::HandleRouteExitPreviewClicked()
{
	UE_LOG(LogTemp, Display, TEXT("[GCSPlayback] Exit Preview clicked"));
	UGameInstance* GameInstance = GetGameInstance();
	URoutePreviewSubsystem* RoutePreview = GameInstance
		? GameInstance->GetSubsystem<URoutePreviewSubsystem>()
		: nullptr;
	if (!RoutePreview)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GCSPlayback] RoutePreviewSubsystem is unavailable"));
		return;
	}

	const bool bWasPreviewActive = RoutePreview->IsPreviewModeActive();
	RoutePreview->ExitPreviewMode();
	if (!bWasPreviewActive)
	{
		UE_LOG(LogTemp, Display, TEXT("[GCSPlayback] Route preview is already exited / Live"));
	}
	RefreshRoutePlaybackUI();
}

void UGCSUIBridgeSubsystem::HandleRouteUnloadClicked()
{
	UE_LOG(LogTemp, Display, TEXT("[GCSPlayback] Unload Route clicked"));
	UGameInstance* GameInstance = GetGameInstance();
	URoutePreviewSubsystem* RoutePreview = GameInstance
		? GameInstance->GetSubsystem<URoutePreviewSubsystem>()
		: nullptr;
	if (!RoutePreview)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GCSPlayback] RoutePreviewSubsystem is unavailable"));
		return;
	}

	RoutePreview->UnloadRoute();
	RefreshRoutePlaybackUI();
}

void UGCSUIBridgeSubsystem::HandleRouteTimelineValueChanged(const float NormalizedValue)
{
	if (bUpdatingRouteTimelineFromState)
	{
		return;
	}

	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview || !RoutePreview->IsRouteLoaded())
	{
		return;
	}

	const int32 LastFrame = FMath::Max(0, RoutePreview->GetFrameCount() - 1);
	const int32 TargetFrame = FMath::RoundToInt(
		FMath::Clamp(NormalizedValue, 0.0f, 1.0f) * LastFrame);
	FString Error;
	if (!RoutePreview->SeekToFrame(TargetFrame, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("[RoutePreviewUI] Seek rejected: %s"), *Error);
	}
	RefreshRoutePlaybackUI();
}

void UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureBegin()
{
	if (URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>())
	{
		if (RoutePreview->GetPlaybackState() == ERoutePreviewPlaybackState::Playing)
		{
			RoutePreview->Pause();
		}
		RefreshRoutePlaybackUI();
	}
}

void UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureEnd()
{
	// Scrubbing intentionally remains paused. The user explicitly resumes with Play.
	RefreshRoutePlaybackUI();
}

void UGCSUIBridgeSubsystem::HandleRouteImportClicked()
{
	OpenRouteImport();
}

void UGCSUIBridgeSubsystem::HandleRouteImportCloseClicked()
{
	CloseRouteImport();
}

void UGCSUIBridgeSubsystem::HandleRouteBrowseClicked()
{
	FString SelectedPath;
	if (!OpenRuntimeNpzFileDialog(SelectedPath))
	{
		return;
	}
	if (!FPaths::GetExtension(SelectedPath).Equals(TEXT("npz"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Selected file is not an NPZ archive."));
		return;
	}

	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview)
	{
		return;
	}

	FString Error;
	if (!RoutePreview->PrepareRouteImport(SelectedPath, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] NPZ import failed: %s"), *Error);
		return;
	}

	SelectedRouteImportFilePath = FPaths::ConvertRelativePathToFull(SelectedPath);
	RefreshRouteImportMetadata();
}

void UGCSUIBridgeSubsystem::HandleRouteLoadPreviewClicked()
{
	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview || SelectedRouteImportFilePath.IsEmpty() || !RoutePreview->HasPreparedRoute())
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Select and validate an NPZ file first."));
		return;
	}

	double SampleRate = 0.0;
	double Latitude = 0.0;
	double Longitude = 0.0;
	double Altitude = 0.0;
	if (!RouteSampleRateInput.IsValid()
		|| !TryParseFiniteDouble(RouteSampleRateInput->GetText(), SampleRate)
		|| SampleRate <= 0.0)
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Sample rate must be a finite number greater than zero."));
		return;
	}
	if (!RouteOriginLatitudeInput.IsValid()
		|| !TryParseFiniteDouble(RouteOriginLatitudeInput->GetText(), Latitude)
		|| Latitude < -90.0
		|| Latitude > 90.0)
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Latitude must be finite and in [-90,90]."));
		return;
	}
	if (!RouteOriginLongitudeInput.IsValid()
		|| !TryParseFiniteDouble(RouteOriginLongitudeInput->GetText(), Longitude)
		|| Longitude < -180.0
		|| Longitude > 180.0)
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Longitude must be finite and in [-180,180]."));
		return;
	}
	if (!RouteOriginAltitudeInput.IsValid()
		|| !TryParseFiniteDouble(RouteOriginAltitudeInput->GetText(), Altitude))
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Altitude must be a finite number in meters."));
		return;
	}

	FString Error;
	if (!RoutePreview->CommitPreparedRoute(Error)
		|| !RoutePreview->SetShowOrigin(Latitude, Longitude, Altitude, Error)
		|| !RoutePreview->SetSampleRateHz(SampleRate, Error)
		|| !RoutePreview->SetRouteOverlayEnabled(true, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Load preview failed: %s"), *Error);
		return;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RouteImport] Import complete; remaining in Live mode with Frame-0 overlay enabled"));
	RefreshRoutePlaybackUI();
	CloseRouteImport();
}

void UGCSUIBridgeSubsystem::HandleRouteSampleRateTextChanged(const FText& Text)
{
	(void)Text;
	RefreshRouteImportMetadata();
}

void UGCSUIBridgeSubsystem::ApplyAndSubmitSelectedDroneLedColor(
	const FLinearColor& PreviewColor,
	const uint8 Red,
	const uint8 Green,
	const uint8 Blue)
{
	if (GetCommandTargetDroneIDs().IsEmpty())
	{
		return;
	}

	ApplySelectedDronePreviewColor(PreviewColor);

	FGCSProductionCommandParameters Parameters;
	Parameters.Red = Red;
	Parameters.Green = Green;
	Parameters.Blue = Blue;
	SubmitSelectedProductionCommand(
		EGCSProductionCommandType::SetLedColor,
		Parameters);
}

void UGCSUIBridgeSubsystem::ApplySelectedDronePreviewColor(const FLinearColor& Color) const
{
	const TArray<FString> SelectedDroneIDs = GetCommandTargetDroneIDs();
	if (SelectedDroneIDs.IsEmpty())
	{
		return;
	}

	UWorld* World = MainWidgetRef.IsValid()
		? MainWidgetRef->GetWorld()
		: GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		It->SetDroneColorsForIDs(SelectedDroneIDs, Color);
		break;
	}
}

void UGCSUIBridgeSubsystem::UpdateTopDownLocateLock(APlayerController* PlayerController)
{
	if (!bTopDownLocateLocked || !PlayerController)
	{
		return;
	}

	if (ACameraActor* TopDownCamera = TopDownOrthographicCamera.Get())
	{
		TopDownCamera->SetActorLocationAndRotation(
			TopDownLockedCameraLocation,
			TopDownLockedCameraRotation,
			false,
			nullptr,
			ETeleportType::TeleportPhysics);
	}
	PlayerController->SetControlRotation(TopDownLockedCameraRotation);
}

void UGCSUIBridgeSubsystem::EndTopDownLocateLock(APlayerController* PlayerController)
{
	const bool bWasTopDownActive = bTopDownLocateLocked;
	APlayerController* RestoreController = TopDownPlayerController.Get();
	if (!RestoreController && !bWasTopDownActive && !PlayerController)
	{
		if (UUserWidget* MainWidget = MainWidgetRef.Get())
		{
			RestoreController = MainWidget->GetOwningPlayer();
		}
	}

	// Disable every TopDown-only update before restoring the normal camera so
	// the centralized input poll cannot overwrite the restored controller state.
	bTopDownLocateLocked = false;
	TopDownFitOrthoWidth = 0.0f;
	bTopDownGeographicBasisActive = false;
	TopDownLocalEast = FVector::XAxisVector;
	TopDownLocalNorth = -FVector::YAxisVector;
	TopDownLocalUp = FVector::ZAxisVector;
	ResetTopDownViewInputState();

	bool bRestoredViewTarget = false;
	bool bRestoredControlRotation = false;
	if (bWasTopDownActive && RestoreController)
	{
		if (PreviousTopDownViewTarget.IsValid())
		{
			RestoreController->SetViewTargetWithBlend(PreviousTopDownViewTarget.Get(), 0.2f);
			bRestoredViewTarget = true;
		}
		if (bHasPreviousTopDownControlRotation)
		{
			RestoreController->SetControlRotation(PreviousTopDownControlRotation);
			bRestoredControlRotation = true;
		}
	}
	if (bWasTopDownActive)
	{
		if (!RestoreController)
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("[GCSLocate] TopDown exit: original PlayerController is unavailable; cached camera state could not be restored"));
		}
		else
		{
			if (bRestoredViewTarget && bRestoredControlRotation)
			{
				UE_LOG(
					LogTemp,
					Log,
					TEXT("[GCSLocate] TopDown exit: Restored ViewTarget=YES Restored ControlRotation=YES"));
			}
			else
			{
				UE_LOG(
					LogTemp,
					Warning,
					TEXT("[GCSLocate] TopDown exit: Restored ViewTarget=%s Restored ControlRotation=%s"),
					bRestoredViewTarget ? TEXT("YES") : TEXT("NO"),
					bRestoredControlRotation ? TEXT("YES") : TEXT("NO"));
			}
		}
	}
	if (UUserWidget* MainWidget = MainWidgetRef.Get())
	{
		if (UWorld* World = MainWidget->GetWorld())
		{
			for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
			{
				It->SetDroneLabelsVisible(false);
				break;
			}
		}
	}
	PreviousTopDownViewTarget.Reset();
	TopDownPlayerController.Reset();
	PreviousTopDownControlRotation = FRotator::ZeroRotator;
	bHasPreviousTopDownControlRotation = false;
}

void UGCSUIBridgeSubsystem::HandleMarqueeMouseButtonDown(
	const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return;
	}

	UUserWidget* MainWidget = MainWidgetRef.Get();
	const FVector2D ScreenPosition = MouseEvent.GetScreenSpacePosition();
	if (!MainWidget
		|| !MainWidget->IsVisible()
		|| !MainWidget->GetCachedGeometry().IsUnderLocation(ScreenPosition)
		|| IsPointerOverBlockingGCSUI(ScreenPosition))
	{
		return;
	}

	bMarqueePointerDown = true;
	bMarqueeSelecting = false;
	MarqueeStartScreenPosition = ScreenPosition;
	MarqueeStartLocalPosition = ScreenToMainWidgetLocal(ScreenPosition);
	UpdateSelectionRectangleVisual(
		MarqueeStartLocalPosition,
		MarqueeStartLocalPosition,
		false);
}

void UGCSUIBridgeSubsystem::HandleMarqueeMouseMove(
	const FPointerEvent& MouseEvent)
{
	if (!bMarqueePointerDown)
	{
		return;
	}

	const FVector2D ScreenPosition = MouseEvent.GetScreenSpacePosition();
	if (!bMarqueeSelecting
		&& FVector2D::Distance(ScreenPosition, MarqueeStartScreenPosition)
			>= MarqueeDragThresholdPixels)
	{
		bMarqueeSelecting = true;
	}

	if (bMarqueeSelecting)
	{
		UpdateSelectionRectangleVisual(
			MarqueeStartLocalPosition,
			ScreenToMainWidgetLocal(ScreenPosition),
			true);
	}
}

void UGCSUIBridgeSubsystem::HandleMarqueeMouseButtonUp(
	const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton
		|| !bMarqueePointerDown)
	{
		return;
	}

	const FVector2D EndPosition =
		ScreenToMainWidgetLocal(MouseEvent.GetScreenSpacePosition());
	if (bMarqueeSelecting)
	{
		UpdateSelectionRectangleVisual(
			MarqueeStartLocalPosition,
			EndPosition,
			true);
		CompleteMarqueeSelection(MarqueeStartLocalPosition, EndPosition);
	}

	UpdateSelectionRectangleVisual(FVector2D::ZeroVector, FVector2D::ZeroVector, false);
	bMarqueePointerDown = false;
	bMarqueeSelecting = false;
}

bool UGCSUIBridgeSubsystem::IsPointerOverBlockingGCSUI(
	const FVector2D& ScreenPosition) const
{
	for (const TWeakObjectPtr<UUserWidget>& WidgetRef :
		{ TopBarWidget,
		  LeftToolbarWidget,
		  FleetPanelWidget,
		  DroneDetailWidget,
		  RightToolbarWidget,
		  RightPanelWidget,
		  PlaybackWidget,
		  MapPageWidget,
		  SettingsPageWidget,
		  LogsPageWidget })
	{
		const UUserWidget* Widget = WidgetRef.Get();
		if (Widget
			&& Widget->IsVisible()
			&& Widget->GetCachedGeometry().IsUnderLocation(ScreenPosition))
		{
			return true;
		}
	}
	if (const UUserWidget* ImportWidget = RouteImportWidget.Get();
		ImportWidget
		&& ImportWidget->IsVisible()
		&& ImportWidget->GetCachedGeometry().IsUnderLocation(ScreenPosition))
	{
		return true;
	}
	return false;
}

FVector2D UGCSUIBridgeSubsystem::ScreenToMainWidgetLocal(
	const FVector2D& ScreenPosition) const
{
	const UUserWidget* MainWidget = MainWidgetRef.Get();
	if (!MainWidget)
	{
		return FVector2D::ZeroVector;
	}

	const FGeometry& Geometry = MainWidget->GetCachedGeometry();
	FVector2D LocalPosition = Geometry.AbsoluteToLocal(ScreenPosition);
	const FVector2D LocalSize = Geometry.GetLocalSize();
	LocalPosition.X = FMath::Clamp(LocalPosition.X, 0.0f, LocalSize.X);
	LocalPosition.Y = FMath::Clamp(LocalPosition.Y, 0.0f, LocalSize.Y);
	return LocalPosition;
}

void UGCSUIBridgeSubsystem::UpdateSelectionRectangleVisual(
	const FVector2D& StartPosition,
	const FVector2D& CurrentPosition,
	const bool bVisible) const
{
	UWidget* Rectangle = SelectionRectangleWidget.Get();
	if (!Rectangle)
	{
		return;
	}

	if (!bVisible)
	{
		Rectangle->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Rectangle->Slot))
	{
		Slot->SetAnchors(FAnchors(0.0f, 0.0f));
		Slot->SetAlignment(FVector2D::ZeroVector);
		Slot->SetZOrder(200);
		const FVector2D Minimum(
			FMath::Min(StartPosition.X, CurrentPosition.X),
			FMath::Min(StartPosition.Y, CurrentPosition.Y));
		const FVector2D Maximum(
			FMath::Max(StartPosition.X, CurrentPosition.X),
			FMath::Max(StartPosition.Y, CurrentPosition.Y));
		Slot->SetPosition(Minimum);
		Slot->SetSize(Maximum - Minimum);
		Slot->SetAutoSize(false);
	}
	Rectangle->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UGCSUIBridgeSubsystem::CompleteMarqueeSelection(
	const FVector2D& StartPosition,
	const FVector2D& EndPosition)
{
	UUserWidget* MainWidget = MainWidgetRef.Get();
	APlayerController* PlayerController = MainWidget ? MainWidget->GetOwningPlayer() : nullptr;
	UWorld* World = MainWidget ? MainWidget->GetWorld() : nullptr;
	UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (!PlayerController || !World || !Fleet || !PlayerController->PlayerCameraManager)
	{
		return;
	}

	ADroneFleetVisualizationActor* Visualization = nullptr;
	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		Visualization = *It;
		break;
	}
	if (!Visualization)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GCS_MARQUEE] Fleet visualization actor not found."));
		return;
	}

	const TArray<FString> DroneIDs = Visualization->GetParticleDroneIDs();
	const TArray<FVector> WorldPositions = Visualization->GetDronePositions();
	const int32 CandidateCount = FMath::Min(DroneIDs.Num(), WorldPositions.Num());
	const FVector2D Minimum(
		FMath::Min(StartPosition.X, EndPosition.X),
		FMath::Min(StartPosition.Y, EndPosition.Y));
	const FVector2D Maximum(
		FMath::Max(StartPosition.X, EndPosition.X),
		FMath::Max(StartPosition.Y, EndPosition.Y));
	const FVector CameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
	const FVector CameraForward =
		PlayerController->PlayerCameraManager->GetCameraRotation().Vector();

	TArray<FString> MarqueeDroneIDs;
	MarqueeDroneIDs.Reserve(CandidateCount);
	for (int32 Index = 0; Index < CandidateCount; ++Index)
	{
		FDroneData Drone;
		const FVector& WorldPosition = WorldPositions[Index];
		if (!Fleet->GetDroneByID(DroneIDs[Index], Drone)
			|| !Drone.bOnline
			|| !DroneGPS::IsValid(Drone)
			|| WorldPosition.ContainsNaN()
			|| FVector::DotProduct(CameraForward, WorldPosition - CameraLocation) <= 0.0f)
		{
			continue;
		}

		FVector2D WidgetPosition;
		if (!UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(
			PlayerController,
			WorldPosition,
			WidgetPosition,
			false))
		{
			continue;
		}

		if (WidgetPosition.X >= Minimum.X
			&& WidgetPosition.X <= Maximum.X
			&& WidgetPosition.Y >= Minimum.Y
			&& WidgetPosition.Y <= Maximum.Y)
		{
			MarqueeDroneIDs.Add(DroneIDs[Index]);
		}
	}

	ReplaceCommandDroneSelection(MarqueeDroneIDs);
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[GCS_MARQUEE] Candidates=%d Selected=%d Rect=(%.1f,%.1f)-(%.1f,%.1f)"),
		CandidateCount,
		MarqueeDroneIDs.Num(),
		Minimum.X,
		Minimum.Y,
		Maximum.X,
		Maximum.Y);
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

void UGCSUIBridgeSubsystem::SyncCommandSelectionToVisualization() const
{
	UWorld* World = MainWidgetRef.IsValid()
		? MainWidgetRef->GetWorld()
		: GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		It->SetCommandSelectedDroneIDs(GetCommandTargetDroneIDs());
		break;
	}
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

UButton* UGCSUIBridgeSubsystem::FindRightPanelButton(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindButton(RightPanelWidget.Get(), Name);
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

void UGCSUIBridgeSubsystem::BindColorPaletteButtons()
{
	if (UButton* Button = FindRightPanelButton(TEXT("BTN_ColorOff")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleColorOffClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleColorOffClicked);
	}
	if (UButton* Button = FindRightPanelButton(TEXT("BTN_ColorWhite")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleColorWhiteClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleColorWhiteClicked);
	}
	if (UButton* Button = FindRightPanelButton(TEXT("BTN_ColorRed")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleColorRedClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleColorRedClicked);
	}
	if (UButton* Button = FindRightPanelButton(TEXT("BTN_ColorGreen")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleColorGreenClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleColorGreenClicked);
	}
	if (UButton* Button = FindRightPanelButton(TEXT("BTN_ColorBlue")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleColorBlueClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleColorBlueClicked);
	}
	if (UButton* Button = FindRightPanelButton(TEXT("BTN_ColorYellow")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleColorYellowClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleColorYellowClicked);
	}
	if (UButton* Button = FindRightPanelButton(TEXT("BTN_ColorMagenta")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleColorMagentaClicked);
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleColorMagentaClicked);
	}
}

void UGCSUIBridgeSubsystem::BindRoutePlaybackControls()
{
	UUserWidget* Playback = PlaybackWidget.Get();
	UWidgetTree* WidgetTree = Playback ? Playback->WidgetTree : nullptr;
	if (!WidgetTree)
	{
		UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] CHILD_Playback widget tree is unavailable."));
		return;
	}

	// BTN_RouteImport is intentionally discovery-only in Phase 1E.
	const bool bRouteImportFound =
		Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_RouteImport"))) != nullptr;
	RouteImportButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_RouteImport")));
	RoutePreviousButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_PreviousFrame")));
	RoutePauseButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_Pause")));
	RoutePlayButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_Play")));
	RouteStopButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_Stop")));
	RouteNextButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_NextFrame")));
	RouteExitPreviewButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_ExitPreview")));
	RouteUnloadButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_UnloadRoute")));
	RouteTimelineSlider = Cast<USlider>(WidgetTree->FindWidget(TEXT("SLD_RouteTimeline")));
	RouteTimeText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TXT_RouteTime")));
	RouteFrameText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TXT_RouteFrame")));

	const bool bRequiredWidgetsFound =
		RoutePreviousButton.IsValid()
		&& RoutePauseButton.IsValid()
		&& RoutePlayButton.IsValid()
		&& RouteStopButton.IsValid()
		&& RouteNextButton.IsValid()
		&& RouteExitPreviewButton.IsValid()
		&& RouteUnloadButton.IsValid()
		&& RouteTimelineSlider.IsValid()
		&& RouteTimeText.IsValid()
		&& RouteFrameText.IsValid();
	if (!bRouteImportFound || !bRequiredWidgetsFound)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("[RoutePreviewUI] Playback widget tree mismatch. Import=%s RequiredControls=%s"),
			bRouteImportFound ? TEXT("YES") : TEXT("NO"),
			bRequiredWidgetsFound ? TEXT("YES") : TEXT("NO"));
		return;
	}

	RouteImportButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteImportClicked);
	RouteImportButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteImportClicked);

	RoutePreviousButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRoutePreviousFrameClicked);
	RoutePreviousButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRoutePreviousFrameClicked);
	RoutePauseButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRoutePauseClicked);
	RoutePauseButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRoutePauseClicked);
	RoutePlayButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRoutePlayClicked);
	RoutePlayButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRoutePlayClicked);
	RouteStopButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteStopClicked);
	RouteStopButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteStopClicked);
	RouteNextButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteNextFrameClicked);
	RouteNextButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteNextFrameClicked);
	RouteExitPreviewButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteExitPreviewClicked);
	RouteExitPreviewButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteExitPreviewClicked);
	RouteUnloadButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteUnloadClicked);
	RouteUnloadButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteUnloadClicked);

	RouteTimelineSlider->OnValueChanged.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineValueChanged);
	RouteTimelineSlider->OnValueChanged.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineValueChanged);
	RouteTimelineSlider->OnMouseCaptureBegin.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureBegin);
	RouteTimelineSlider->OnMouseCaptureBegin.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureBegin);
	RouteTimelineSlider->OnMouseCaptureEnd.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureEnd);
	RouteTimelineSlider->OnMouseCaptureEnd.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureEnd);
	RouteTimelineSlider->OnControllerCaptureBegin.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureBegin);
	RouteTimelineSlider->OnControllerCaptureBegin.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureBegin);
	RouteTimelineSlider->OnControllerCaptureEnd.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureEnd);
	RouteTimelineSlider->OnControllerCaptureEnd.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleRouteTimelineCaptureEnd);

	RouteUIRefreshAccumulator = 0.0f;
	RefreshRoutePlaybackUI();
	UE_LOG(LogTemp, Display, TEXT("[RoutePreviewUI] Playback controls and RouteImport entry bound."));
}

void UGCSUIBridgeSubsystem::OpenRouteImport()
{
	UUserWidget* ImportWidget = RouteImportWidget.Get();
	if (!ImportWidget)
	{
		UUserWidget* Owner = MainWidgetRef.Get();
		APlayerController* PlayerController = Owner ? Owner->GetOwningPlayer() : nullptr;
		const TSubclassOf<UUserWidget> ImportClass = RouteImportWidgetClass;
		if (!PlayerController || !ImportClass)
		{
			UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] Unable to load WBP_GCS_RouteImport."));
			return;
		}

		ImportWidget = CreateWidget<UUserWidget>(PlayerController, ImportClass);
		if (!ImportWidget || !BindRouteImportControls(ImportWidget))
		{
			UE_LOG(LogTemp, Error, TEXT("[RoutePreviewUI] RouteImport widget tree binding failed."));
			return;
		}
		UWidget* RootWidget = ImportWidget->WidgetTree
			? ImportWidget->WidgetTree->RootWidget.Get()
			: nullptr;
		UWidget* ImportBorder = ImportWidget->WidgetTree
			? ImportWidget->WidgetTree->FindWidget(TEXT("BORDER_RouteImport"))
			: nullptr;
		UCanvasPanelSlot* ImportBorderSlot = ImportBorder
			? Cast<UCanvasPanelSlot>(ImportBorder->Slot)
			: nullptr;
		if (!Cast<UCanvasPanel>(RootWidget) || !ImportBorderSlot)
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("[RoutePreviewUI] RouteImport requires Canvas_RouteImport root and BORDER_RouteImport Canvas slot."));
			return;
		}

		// AddToViewport's default slot stretches the root canvas across the live
		// viewport. Center only the authored 420x400 border inside that canvas;
		// this keeps layout in UMG/DPI space and avoids resolution-specific offsets.
		ImportBorderSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		ImportBorderSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		ImportBorderSlot->SetPosition(FVector2D::ZeroVector);
		ImportBorderSlot->SetAutoSize(false);
		ImportWidget->AddToViewport(300);
		RouteImportWidget = ImportWidget;
	}

	ImportWidget->SetVisibility(ESlateVisibility::Visible);
	RefreshRouteImportMetadata();
}

void UGCSUIBridgeSubsystem::CloseRouteImport()
{
	if (UUserWidget* ImportWidget = RouteImportWidget.Get())
	{
		ImportWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
}

bool UGCSUIBridgeSubsystem::BindRouteImportControls(UUserWidget* ImportWidget)
{
	UWidgetTree* WidgetTree = ImportWidget ? ImportWidget->WidgetTree : nullptr;
	if (!WidgetTree)
	{
		return false;
	}

	RouteImportCloseButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_RouteImportClose")));
	RouteBrowseButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_RouteBrowse")));
	RouteCancelButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_RouteCancel")));
	RouteLoadPreviewButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BTN_RouteLoadPreview")));
	RouteSampleRateInput = Cast<UEditableTextBox>(WidgetTree->FindWidget(TEXT("EDT_SampleRate")));
	RouteOriginLatitudeInput = Cast<UEditableTextBox>(WidgetTree->FindWidget(TEXT("EDT_OriginLatitude")));
	RouteOriginLongitudeInput = Cast<UEditableTextBox>(WidgetTree->FindWidget(TEXT("EDT_OriginLongitude")));
	RouteOriginAltitudeInput = Cast<UEditableTextBox>(WidgetTree->FindWidget(TEXT("EDT_OriginAltitude")));
	RouteImportFilePathText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TXT_RouteFilePath")));
	RouteImportDroneValueText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TXT_RouteDroneValue")));
	RouteImportFrameCountValueText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TXT_RouteFrameCountValue")));
	RouteImportDurationValueText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TXT_RouteDurationValue")));

	if (!RouteImportCloseButton.IsValid()
		|| !RouteBrowseButton.IsValid()
		|| !RouteCancelButton.IsValid()
		|| !RouteLoadPreviewButton.IsValid()
		|| !RouteSampleRateInput.IsValid()
		|| !RouteOriginLatitudeInput.IsValid()
		|| !RouteOriginLongitudeInput.IsValid()
		|| !RouteOriginAltitudeInput.IsValid()
		|| !RouteImportFilePathText.IsValid()
		|| !RouteImportDroneValueText.IsValid()
		|| !RouteImportFrameCountValueText.IsValid()
		|| !RouteImportDurationValueText.IsValid())
	{
		return false;
	}

	RouteImportCloseButton->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteImportCloseClicked);
	RouteImportCloseButton->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteImportCloseClicked);
	RouteCancelButton->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteImportCloseClicked);
	RouteCancelButton->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteImportCloseClicked);
	RouteBrowseButton->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteBrowseClicked);
	RouteBrowseButton->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteBrowseClicked);
	RouteLoadPreviewButton->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteLoadPreviewClicked);
	RouteLoadPreviewButton->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteLoadPreviewClicked);
	RouteSampleRateInput->OnTextChanged.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteSampleRateTextChanged);
	RouteSampleRateInput->OnTextChanged.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleRouteSampleRateTextChanged);
	return true;
}

void UGCSUIBridgeSubsystem::RefreshRouteImportMetadata()
{
	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	const bool bPrepared = RoutePreview && RoutePreview->HasPreparedRoute();
	if (UTextBlock* FileText = RouteImportFilePathText.Get())
	{
		FileText->SetText(FText::FromString(
			bPrepared ? FPaths::GetCleanFilename(SelectedRouteImportFilePath) : TEXT("...")));
	}
	if (UTextBlock* DroneText = RouteImportDroneValueText.Get())
	{
		DroneText->SetText(FText::FromString(
			bPrepared ? FString::FromInt(RoutePreview->GetPreparedDroneCount()) : TEXT("--")));
	}
	if (UTextBlock* FrameText = RouteImportFrameCountValueText.Get())
	{
		FrameText->SetText(FText::FromString(
			bPrepared ? FString::FromInt(RoutePreview->GetPreparedFrameCount()) : TEXT("--")));
	}
	if (UTextBlock* DurationText = RouteImportDurationValueText.Get())
	{
		double SampleRate = 0.0;
		const bool bValidRate = RouteSampleRateInput.IsValid()
			&& TryParseFiniteDouble(RouteSampleRateInput->GetText(), SampleRate)
			&& SampleRate > 0.0;
		DurationText->SetText(FText::FromString(
			bPrepared && bValidRate
				? FormatRouteDuration((RoutePreview->GetPreparedFrameCount() - 1) / SampleRate)
				: TEXT("--")));
	}
}

void UGCSUIBridgeSubsystem::StepRoutePreviewFrame(const int32 Direction)
{
	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview || !RoutePreview->IsRouteLoaded())
	{
		return;
	}

	if (RoutePreview->GetPlaybackState() == ERoutePreviewPlaybackState::Playing)
	{
		RoutePreview->Pause();
	}
	const int32 LastFrame = FMath::Max(0, RoutePreview->GetFrameCount() - 1);
	const int32 CurrentFrame = FMath::Max(0, RoutePreview->GetCurrentFrame());
	const int32 TargetFrame = FMath::Clamp(CurrentFrame + Direction, 0, LastFrame);
	FString Error;
	if (!RoutePreview->SeekToFrame(TargetFrame, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("[RoutePreviewUI] Frame step rejected: %s"), *Error);
	}
	RefreshRoutePlaybackUI();
}

void UGCSUIBridgeSubsystem::TickRoutePlaybackUI(const float DeltaSeconds)
{
	RouteUIRefreshAccumulator += FMath::Max(0.0f, DeltaSeconds);
	if (RouteUIRefreshAccumulator < RouteUIRefreshIntervalSeconds)
	{
		return;
	}
	RouteUIRefreshAccumulator = FMath::Fmod(
		RouteUIRefreshAccumulator,
		RouteUIRefreshIntervalSeconds);
	RefreshRoutePlaybackUI();
}

void UGCSUIBridgeSubsystem::RefreshRoutePlaybackUI()
{
	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview)
	{
		return;
	}

	const bool bLoaded = RoutePreview->IsRouteLoaded();
	const ERoutePreviewPlaybackState State = RoutePreview->GetPlaybackState();
	const bool bPlaying = State == ERoutePreviewPlaybackState::Playing;
	const bool bCompleted = State == ERoutePreviewPlaybackState::Completed;

	if (UButton* Button = RoutePreviousButton.Get())
	{
		Button->SetIsEnabled(bLoaded && !bPlaying);
	}
	if (UButton* Button = RoutePauseButton.Get())
	{
		Button->SetIsEnabled(bLoaded && bPlaying);
	}
	if (UButton* Button = RoutePlayButton.Get())
	{
		Button->SetIsEnabled(bLoaded && !bPlaying);
	}
	if (UButton* Button = RouteStopButton.Get())
	{
		Button->SetIsEnabled(bLoaded);
	}
	if (UButton* Button = RouteNextButton.Get())
	{
		Button->SetIsEnabled(bLoaded && !bPlaying && !bCompleted);
	}
	if (UButton* Button = RouteExitPreviewButton.Get())
	{
		Button->SetIsEnabled(bLoaded);
	}
	if (UButton* Button = RouteUnloadButton.Get())
	{
		Button->SetIsEnabled(bLoaded);
	}

	const int32 FrameCount = bLoaded ? RoutePreview->GetFrameCount() : 0;
	const int32 CurrentFrame = FrameCount > 0
		? FMath::Clamp(FMath::Max(0, RoutePreview->GetCurrentFrame()), 0, FrameCount - 1)
		: 0;
	if (USlider* Slider = RouteTimelineSlider.Get())
	{
		Slider->SetIsEnabled(bLoaded);
		const float NormalizedValue = FrameCount > 1
			? static_cast<float>(CurrentFrame) / static_cast<float>(FrameCount - 1)
			: 0.0f;
		bUpdatingRouteTimelineFromState = true;
		Slider->SetValue(NormalizedValue);
		bUpdatingRouteTimelineFromState = false;
	}

	if (UTextBlock* FrameText = RouteFrameText.Get())
	{
		FrameText->SetText(FText::FromString(FString::Printf(
			TEXT("Frame %d / %d"),
			CurrentFrame,
			FrameCount)));
	}

	if (UTextBlock* TimeText = RouteTimeText.Get())
	{
		FString TimeDisplay = TEXT("--:-- / --:--");
		if (bLoaded && RoutePreview->GetSampleRateHz() > 0.0)
		{
			const auto FormatMinutesSeconds = [](const double Seconds)
			{
				const int32 TotalSeconds = FMath::Max(0, FMath::FloorToInt(Seconds));
				return FString::Printf(
					TEXT("%02d:%02d"),
					TotalSeconds / 60,
					TotalSeconds % 60);
			};
			TimeDisplay = FString::Printf(
				TEXT("%s / %s"),
				*FormatMinutesSeconds(RoutePreview->GetCurrentTime()),
				*FormatMinutesSeconds(RoutePreview->GetDurationSeconds()));
		}
		TimeText->SetText(FText::FromString(TimeDisplay));
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
	const bool bToggleOffActiveNavigationPage =
		ActivePage == Page
		&& (Page == EGCSMainPage::Map
			|| Page == EGCSMainPage::Settings
			|| Page == EGCSMainPage::Logs);
	ActivePage = bToggleOffActiveNavigationPage ? EGCSMainPage::Fleet : Page;
	bPanelsExpanded = true;
	ApplyNavigationPageVisibility();
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
	ApplyNavigationPageVisibility();

	UE_LOG(LogTemp, Log, TEXT("GCS UI navigation changed to page %d"), static_cast<int32>(ActivePage));
}

void UGCSUIBridgeSubsystem::ApplyNavigationPageVisibility()
{
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
