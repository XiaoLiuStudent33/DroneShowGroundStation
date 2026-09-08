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
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "DSPGCSProductionCommandProviderSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "DSPGCSUploadJobSubsystem.h"
#include "DroneFleetVisualizationActor.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"
#include "FleetManagerSubsystem.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GCSBenchmarkExport.h"
#include "GCSDroneRowView.h"
#include "GCSFleetRowCheckBinding.h"
#include "GCSRouteWorkflowCoordinator.h"
#include "GCSTelemetryFilterController.h"
#include "HoverLayerSelectionModel.h"
#include "GCSUIWidgetPresenter.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Math/RotationMatrix.h"
#include "Layout/WidgetPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "RoutePreviewSubsystem.h"
#include "RouteArtifactWorkspaceSubsystem.h"
#include "RouteAssignmentGeo.h"
#include "RouteAssignmentSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWidget.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShObjIdl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	constexpr TCHAR UploadDashboardClassPath[] =
		TEXT("/Game/UI/WBP_GCS_UploadDashboard.WBP_GCS_UploadDashboard_C");
	constexpr float UploadDashboardToolbarGap = 12.0f;
	constexpr float FixedPileAxisLengthMeters = 1000.0f;
	constexpr float FixedPileAxisDepthOffsetUU = 300.0f;
	const FName SceneMapActorTag(TEXT("GCS.SceneMap"));

	bool IsVisibleWidgetUnderLocation(
		const UWidget* Widget,
		const FVector2D& ScreenPosition)
	{
		if (!Widget || !Widget->IsVisible())
		{
			return false;
		}

		const FGeometry& Geometry = Widget->GetCachedGeometry();
		const FVector2D LocalSize = Geometry.GetLocalSize();
		return LocalSize.X > KINDA_SMALL_NUMBER
			&& LocalSize.Y > KINDA_SMALL_NUMBER
			&& Geometry.IsUnderLocation(ScreenPosition);
	}

	bool IsPointerOverInteractableSlateWidget(const FVector2D& ScreenPosition)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}

		FSlateApplication& SlateApplication = FSlateApplication::Get();
		const FWidgetPath WidgetPath = SlateApplication.LocateWindowUnderMouse(
			ScreenPosition,
			SlateApplication.GetInteractiveTopLevelWindows());
		for (int32 WidgetIndex = 0; WidgetIndex < WidgetPath.Widgets.Num(); ++WidgetIndex)
		{
			const FArrangedWidget& ArrangedWidget = WidgetPath.Widgets[WidgetIndex];
			if (ArrangedWidget.Widget->IsInteractable())
			{
				return true;
			}
		}
		return false;
	}

	bool HaveSameDroneIDs(const TArray<FString>& Left, const TArray<FString>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		TArray<FString> SortedLeft = Left;
		TArray<FString> SortedRight = Right;
		SortedLeft.Sort();
		SortedRight.Sort();
		return SortedLeft == SortedRight;
	}

	int32 ParseLogicalDroneNumber(const FString& DroneID)
	{
		int32 DigitStart = DroneID.Len();
		while (DigitStart > 0 && FChar::IsDigit(DroneID[DigitStart - 1]))
		{
			--DigitStart;
		}
		return DigitStart < DroneID.Len()
			? FCString::Atoi(*DroneID.Mid(DigitStart))
			: INDEX_NONE;
	}

	bool SetTelemetryInteger(UObject* Object, const FName Name, const int32 Value)
	{
		if (FIntProperty* Property = FindFProperty<FIntProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		return false;
	}

	bool SetTelemetryInteger64(UObject* Object, const FName Name, const int64 Value)
	{
		if (FInt64Property* Property = FindFProperty<FInt64Property>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		return false;
	}

	bool SetTelemetryString(UObject* Object, const FName Name, const FString& Value)
	{
		if (FStrProperty* Property = FindFProperty<FStrProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		return false;
	}

	bool SetFleetString(
		UObject* Object,
		const FName Name,
		const FString& Value,
		bool& bOutChanged)
	{
		if (FStrProperty* Property = FindFProperty<FStrProperty>(Object->GetClass(), Name))
		{
			if (Property->GetPropertyValue_InContainer(Object) != Value)
			{
				Property->SetPropertyValue_InContainer(Object, Value);
				bOutChanged = true;
			}
			return true;
		}
		return false;
	}

	bool SetFleetBool(
		UObject* Object,
		const FName Name,
		const bool Value,
		bool& bOutChanged)
	{
		if (FBoolProperty* Property = FindFProperty<FBoolProperty>(Object->GetClass(), Name))
		{
			if (Property->GetPropertyValue_InContainer(Object) != Value)
			{
				Property->SetPropertyValue_InContainer(Object, Value);
				bOutChanged = true;
			}
			return true;
		}
		return false;
	}

	bool GetFleetString(UObject* Object, const FName Name, FString& OutValue)
	{
		if (Object)
		{
			if (const FStrProperty* Property =
				FindFProperty<FStrProperty>(Object->GetClass(), Name))
			{
				OutValue = Property->GetPropertyValue_InContainer(Object);
				return true;
			}
		}
		OutValue.Reset();
		return false;
	}

	bool SetTelemetryReal(UObject* Object, const FName Name, const double Value)
	{
		if (FDoubleProperty* Property = FindFProperty<FDoubleProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		if (FFloatProperty* Property = FindFProperty<FFloatProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, static_cast<float>(Value));
			return true;
		}
		return false;
	}

	bool IsSceneMapActor(const AActor* Actor)
	{
		return Actor
			&& (Actor->ActorHasTag(SceneMapActorTag)
				|| Actor->GetClass()->GetName().StartsWith(TEXT("BP_GCS_2DMapGround")));
	}

	bool SetVectorFunctionParameter(
		UFunction* Function,
		void* ParameterMemory,
		const FName Name,
		const FVector& Value)
	{
		FStructProperty* Property = FindFProperty<FStructProperty>(Function, Name);
		if (!Property || Property->Struct != TBaseStructure<FVector>::Get())
		{
			return false;
		}
		Property->CopyCompleteValue(
			Property->ContainerPtrToValuePtr<void>(ParameterMemory),
			&Value);
		return true;
	}

	FString FormatTimeControlDuration(const int32 TotalSeconds)
	{
		const int32 ClampedSeconds = FMath::Max(0, TotalSeconds);
		return FString::Printf(
			TEXT("%02d:%02d:%02d"),
			ClampedSeconds / 3600,
			(ClampedSeconds % 3600) / 60,
			ClampedSeconds % 60);
	}

	template <typename T>
	T* FindDashboardWidget(UUserWidget* Dashboard, const FName Name)
	{
		return Dashboard && Dashboard->WidgetTree
			? Cast<T>(Dashboard->WidgetTree->FindWidget(Name))
			: nullptr;
	}

	void SetDashboardText(
		UUserWidget* Dashboard,
		const FName Name,
		const FString& Value,
		const FString& ToolTip = {})
	{
		if (UTextBlock* Text = FindDashboardWidget<UTextBlock>(Dashboard, Name))
		{
			Text->SetText(FText::FromString(Value));
			Text->SetToolTipText(FText::FromString(ToolTip));
		}
	}

	bool ParseDashboardGlobalCap(const FText& Text, int32& OutValue)
	{
		const FString Trimmed = Text.ToString().TrimStartAndEnd();
		int64 Parsed = 0;
		if (Trimmed.IsEmpty() || !LexTryParseString(Parsed, *Trimmed) ||
			Parsed < 1 || Parsed > 800)
		{
			return false;
		}
		OutValue = static_cast<int32>(Parsed);
		return true;
	}

	void SetDashboardInputText(UWidget* Widget, const FString& Value)
	{
		if (UEditableText* Input = Cast<UEditableText>(Widget))
			Input->SetText(FText::FromString(Value));
		else if (UEditableTextBox* InputBox = Cast<UEditableTextBox>(Widget))
			InputBox->SetText(FText::FromString(Value));
	}

	FText GetDashboardInputText(const UWidget* Widget)
	{
		if (const UEditableText* Input = Cast<UEditableText>(Widget))
			return Input->GetText();
		if (const UEditableTextBox* InputBox = Cast<UEditableTextBox>(Widget))
			return InputBox->GetText();
		return FText::GetEmpty();
	}

	void SetDashboardInputReadOnly(UWidget* Widget, const bool bReadOnly)
	{
		if (UEditableText* Input = Cast<UEditableText>(Widget))
			Input->SetIsReadOnly(bReadOnly);
		else if (UEditableTextBox* InputBox = Cast<UEditableTextBox>(Widget))
			InputBox->SetIsReadOnly(bReadOnly);
	}

	bool ResolveAuthoredCanvasSize(UUserWidget* Widget, FVector2D& OutSize)
	{
		OutSize = FVector2D::ZeroVector;
		if (!Widget || !Widget->WidgetTree)
		{
			return false;
		}

		Widget->ForceLayoutPrepass();
		const FVector2D DesiredSize = Widget->GetDesiredSize();
		if (DesiredSize.X > 1.0f && DesiredSize.Y > 1.0f)
		{
			OutSize = DesiredSize;
			return true;
		}

		const UCanvasPanel* RootCanvas =
			Cast<UCanvasPanel>(Widget->WidgetTree->RootWidget);
		if (!RootCanvas)
		{
			return false;
		}

		FVector2D MaximumExtent = FVector2D::ZeroVector;
		for (int32 ChildIndex = 0; ChildIndex < RootCanvas->GetChildrenCount(); ++ChildIndex)
		{
			const UWidget* Child = RootCanvas->GetChildAt(ChildIndex);
			const UCanvasPanelSlot* ChildSlot = Child
				? Cast<UCanvasPanelSlot>(Child->Slot)
				: nullptr;
			if (!ChildSlot)
			{
				continue;
			}

			const FAnchors ChildAnchors = ChildSlot->GetAnchors();
			if (!ChildAnchors.Minimum.Equals(FVector2D::ZeroVector)
				|| !ChildAnchors.Maximum.Equals(FVector2D::ZeroVector))
			{
				continue;
			}

			const FVector2D ChildSize = ChildSlot->GetAutoSize()
				? Child->GetDesiredSize()
				: ChildSlot->GetSize();
			const FVector2D ChildTopLeft = ChildSlot->GetPosition()
				- (ChildSize * ChildSlot->GetAlignment());
			MaximumExtent.X = FMath::Max(MaximumExtent.X, ChildTopLeft.X + ChildSize.X);
			MaximumExtent.Y = FMath::Max(MaximumExtent.Y, ChildTopLeft.Y + ChildSize.Y);
		}

		if (MaximumExtent.X <= 1.0f || MaximumExtent.Y <= 1.0f)
		{
			return false;
		}
		OutSize = MaximumExtent;
		return true;
	}

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

	enum class ERuntimeSaveDialogResult : uint8
	{
		Selected,
		Cancelled,
		Failed
	};

	ERuntimeSaveDialogResult OpenRuntimeBenchmarkSaveDialog(FString& OutFilePath)
	{
		OutFilePath.Reset();
#if PLATFORM_WINDOWS
		const HRESULT InitializeResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const bool bUninitializeCom = SUCCEEDED(InitializeResult);
		if (FAILED(InitializeResult) && InitializeResult != RPC_E_CHANGED_MODE)
		{
			return ERuntimeSaveDialogResult::Failed;
		}

		ERuntimeSaveDialogResult Outcome = ERuntimeSaveDialogResult::Failed;
		IFileSaveDialog* Dialog = nullptr;
		HRESULT Result = ::CoCreateInstance(
			CLSID_FileSaveDialog,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&Dialog));
		if (SUCCEEDED(Result) && Dialog)
		{
			const COMDLG_FILTERSPEC Filters[] =
			{
				{ L"Benchmark text files (*.txt)", L"*.txt" }
			};
			Dialog->SetFileTypes(UE_ARRAY_COUNT(Filters), Filters);
			Dialog->SetFileTypeIndex(1);
			Dialog->SetDefaultExtension(L"txt");
			Dialog->SetFileName(L"benchmark.txt");

			FILEOPENDIALOGOPTIONS Options = {};
			if (SUCCEEDED(Dialog->GetOptions(&Options)))
			{
				Dialog->SetOptions(
					Options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
			}

			const HWND ParentWindow = FSlateApplication::IsInitialized()
				? static_cast<HWND>(const_cast<void*>(
					FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)))
				: nullptr;
			Result = Dialog->Show(ParentWindow);
			if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
			{
				Outcome = ERuntimeSaveDialogResult::Cancelled;
			}
			else if (SUCCEEDED(Result))
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
						Outcome = ERuntimeSaveDialogResult::Selected;
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
		return Outcome;
#else
		return ERuntimeSaveDialogResult::Failed;
#endif
	}

	bool OpenRuntimeFolderDialog(FString& OutFolderPath)
	{
		OutFolderPath.Reset();
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
			FILEOPENDIALOGOPTIONS Options = {};
			if (SUCCEEDED(Dialog->GetOptions(&Options)))
			{
				Dialog->SetOptions(Options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST
					| FOS_FORCEFILESYSTEM);
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
					if (SUCCEEDED(SelectedItem->GetDisplayName(
						SIGDN_FILESYSPATH, &SelectedPath)) && SelectedPath)
					{
						OutFolderPath = SelectedPath;
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
		return !OutFolderPath.IsEmpty();
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
			Bridge->TickTimeControlUI(DeltaTime);
			Bridge->TickUploadDashboardUI(DeltaTime);
			Bridge->TickTelemetryTableUI(DeltaTime);
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
	UnbindFleetList();
	ResetNavigationPages();
	UnbindUploadDashboardControls();
	UnbindReferenceAndMapControls();
	UnbindHoverTestControls();

	MainWidgetRef = MainWidget;
	ConfigureGroundStationInputPolicy(MainWidget);
	TopBarWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_TopBar"));
	LeftToolbarWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_LeftToolbar"));
	FleetPanelWidget = FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_FleetPanel"));
	RightToolbarWidget =
		FGCSUIWidgetPresenter::ResolveChildWidget(MainWidget, TEXT("CHILD_RightToolbar"));
	BindTelemetryFilterToggle();
	BindUploadDashboardControls();
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
	if (!RouteWorkflowCoordinator)
	{
		RouteWorkflowCoordinator = NewObject<UGCSRouteWorkflowCoordinator>(this);
	}
	if (!RouteWorkflowCoordinator->Initialize(GetGameInstance(), RightPanelWidget.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("Route Workflow coordinator initialization failed."));
	}
	CreateNavigationPages();
	BindNavigationButtons();
	bPanelsExpanded = true;
	bFleetPanelRequestedVisible = false;
	bDroneDetailRequestedVisible = false;
	ApplyNavigationVisibility();
	FGCSUIWidgetPresenter::SetWidgetVisible(RightPanelWidget, false);
	FGCSUIWidgetPresenter::SetWidgetVisible(PlaybackWidget, false);
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
	BindTimeControlControls();
	BindReferenceAndMapControls();
	BindRoutePlaybackControls();
	BindFleetPanelButtons();
	BindHoverTestControls();
	SetLegacyHoverSelectionVisible(false);
	RefreshHoverMVPControls();
	RefreshHoverTestControls();

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
	UnbindFleetList();
	EndTopDownLocateLock();
	UnbindHoverTestControls();
	UnbindReferenceAndMapControls();
	ResetUploadDashboard();
	if (UButton* SelectAllButton = FindFleetButton(TEXT("BTN_FleetSelectALL")))
	{
		SelectAllButton->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleFleetSelectAllClicked);
	}
	if (RouteWorkflowCoordinator)
	{
		RouteWorkflowCoordinator->Shutdown();
		RouteWorkflowCoordinator = nullptr;
	}
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
	HoverTestContentWidget.Reset();
	HoverAltitudeInput.Reset();
	HoverLayerCombo.Reset();
	HoverGenerateLayersButton.Reset();
	HoverLayers.Reset();
	HoverLayerBaseDroneIDs.Reset();
	HoverLayerOptions.Reset();
	HoverLayerFleetSessionID.Reset();
	HoverLayerPositionSource.Reset();
	HoverTestStatus.Reset();
	ActiveHoverLayerOptionIndex = INDEX_NONE;
	bUpdatingHoverLayerCombo = false;
	bApplyingHoverLayerSelection = false;
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
	FleetListRowObjects.Reset();
	FleetListCheckBindings.Reset();
	TelemetryRowObjects.Reset();
	FleetPageOffset = 0;
	bMarqueePointerDown = false;
	bMarqueeSelecting = false;
	bMarqueeLeftMouseWasDown = false;
	MarqueeCurrentLocalPosition = FVector2D::ZeroVector;
	bUpdatingRouteTimelineFromState = false;
	RouteUIRefreshAccumulator = 0.0f;
	TimeControlUIRefreshAccumulator = 0.0f;
	TelemetryTableRefreshAccumulator = 0.0f;
	ResetTimeControlPreparationState();

	Super::Deinitialize();
}

void UGCSUIBridgeSubsystem::BindUploadDashboardControls()
{
	UnbindUploadDashboardControls();

	UButton* ToggleButton =
		FGCSUIWidgetPresenter::FindButton(RightToolbarWidget.Get(), TEXT("BTN_Device"));
	if (!ToggleButton)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("Upload Dashboard toggle is unavailable: BTN_Device was not found."));
		return;
	}

	UploadDashboardToggleButton = ToggleButton;
	ToggleButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleUploadDashboardToggleClicked);
	ToggleButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleUploadDashboardToggleClicked);
	UE_LOG(LogTemp, Log, TEXT("[UploadDashboard] BTN_Device bound."));

	if (UploadDashboardWidget)
	{
		if (!AttachUploadDashboardToMainLayout())
		{
			bUploadDashboardVisible = false;
		}
		ApplyUploadDashboardVisibility();
	}
}

void UGCSUIBridgeSubsystem::UnbindUploadDashboardControls()
{
	if (UButton* ToggleButton = UploadDashboardToggleButton.Get())
	{
		ToggleButton->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleUploadDashboardToggleClicked);
	}
	UploadDashboardToggleButton.Reset();
}

bool UGCSUIBridgeSubsystem::CreateUploadDashboardIfNeeded()
{
	if (UploadDashboardWidget)
	{
		return AttachUploadDashboardToMainLayout();
	}

	UUserWidget* NewDashboard = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
	if (UploadDashboardFactoryForTest)
	{
		NewDashboard = UploadDashboardFactoryForTest();
	}
	else
#endif
	{
		const TSubclassOf<UUserWidget> DashboardClass =
			LoadClass<UUserWidget>(nullptr, UploadDashboardClassPath);
		if (!DashboardClass)
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("Unable to load Upload Dashboard class: %s"),
				UploadDashboardClassPath);
			return false;
		}

		NewDashboard = CreateWidget<UUserWidget>(
			GetGameInstance(),
			DashboardClass,
			TEXT("RuntimeUploadDashboard"));
	}

	if (!NewDashboard)
	{
		UE_LOG(LogTemp, Error, TEXT("Unable to create the Upload Dashboard widget."));
		return false;
	}

	UploadDashboardWidget = NewDashboard;
	UploadDashboardWidget->SetVisibility(ESlateVisibility::Collapsed);
	if (!AttachUploadDashboardToMainLayout())
	{
		UploadDashboardWidget->RemoveFromParent();
		UploadDashboardWidget = nullptr;
		return false;
	}
	BindUploadDashboardDataControls();
	UE_LOG(
		LogTemp,
		Log,
		TEXT("[UploadDashboard] Created class=%s host=MainRootCanvas."),
		*GetNameSafe(UploadDashboardWidget->GetClass()));
	return true;
}

bool UGCSUIBridgeSubsystem::AttachUploadDashboardToMainLayout()
{
	UUserWidget* MainWidget = MainWidgetRef.Get();
	UUserWidget* Dashboard = UploadDashboardWidget;
	if (!MainWidget || !MainWidget->WidgetTree || !Dashboard)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Unable to attach Upload Dashboard: main widget or dashboard is unavailable."));
		return false;
	}

	UCanvasPanel* MainCanvas = Cast<UCanvasPanel>(MainWidget->WidgetTree->RootWidget);
	UCanvasPanelSlot* ToolbarSlot = RightToolbarWidget.IsValid()
		? Cast<UCanvasPanelSlot>(RightToolbarWidget->Slot)
		: nullptr;
	if (!MainCanvas || !ToolbarSlot)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Unable to attach Upload Dashboard: main root or RightToolbar is not Canvas-backed."));
		return false;
	}

	const FAnchors ToolbarAnchors = ToolbarSlot->GetAnchors();
	if (!ToolbarAnchors.Minimum.Equals(ToolbarAnchors.Maximum))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Unable to attach Upload Dashboard: stretched RightToolbar anchors are unsupported."));
		return false;
	}

	if (Dashboard->GetParent() != MainCanvas)
	{
		Dashboard->RemoveFromParent();
		if (!MainCanvas->AddChildToCanvas(Dashboard))
		{
			UE_LOG(LogTemp, Error, TEXT("Unable to attach Upload Dashboard to the main Canvas."));
			return false;
		}
	}

	UCanvasPanelSlot* DashboardSlot = Cast<UCanvasPanelSlot>(Dashboard->Slot);
	if (!DashboardSlot)
	{
		UE_LOG(LogTemp, Error, TEXT("Upload Dashboard did not receive a Canvas slot."));
		return false;
	}

	const FVector2D ToolbarTopLeft = ToolbarSlot->GetPosition()
		- (ToolbarSlot->GetSize() * ToolbarSlot->GetAlignment());
	FVector2D DashboardSize = FVector2D::ZeroVector;
	if (!ResolveAuthoredCanvasSize(Dashboard, DashboardSize))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Unable to attach Upload Dashboard: authored panel size is unavailable."));
		return false;
	}
	DashboardSlot->SetAnchors(ToolbarAnchors);
	DashboardSlot->SetAlignment(FVector2D(1.0f, 0.0f));
	DashboardSlot->SetPosition(FVector2D(
		ToolbarTopLeft.X - UploadDashboardToolbarGap,
		ToolbarTopLeft.Y));
	DashboardSlot->SetAutoSize(false);
	DashboardSlot->SetSize(DashboardSize);
	DashboardSlot->SetZOrder(ToolbarSlot->GetZOrder() + 1);
	Dashboard->ForceLayoutPrepass();
	UE_LOG(
		LogTemp,
		Log,
		TEXT("[UploadDashboard] Attached size=(%.0f,%.0f) toolbarGap=%.0f."),
		DashboardSize.X,
		DashboardSize.Y,
		UploadDashboardToolbarGap);
	return true;
}

void UGCSUIBridgeSubsystem::HandleUploadDashboardToggleClicked()
{
#if WITH_DEV_AUTOMATION_TESTS
	++UploadDashboardToggleCountForTest;
#endif
	if (!bUploadDashboardVisible)
	{
		if (!CreateUploadDashboardIfNeeded())
		{
			bUploadDashboardVisible = false;
			ApplyUploadDashboardVisibility();
			return;
		}
		bUploadDashboardVisible = true;
	}
	else
	{
		bUploadDashboardVisible = false;
	}
	ApplyUploadDashboardVisibility();
	UE_LOG(
		LogTemp,
		Log,
		TEXT("[UploadDashboard] Visibility=%s instance=%s."),
		bUploadDashboardVisible ? TEXT("Visible") : TEXT("Collapsed"),
		*GetNameSafe(UploadDashboardWidget));
}

void UGCSUIBridgeSubsystem::ApplyUploadDashboardVisibility()
{
	if (UploadDashboardWidget)
	{
		UploadDashboardWidget->SetVisibility(
			bUploadDashboardVisible
				? ESlateVisibility::Visible
				: ESlateVisibility::Collapsed);
		if (bUploadDashboardVisible)
		{
			if (UDSPGCSUploadJobSubsystem* Upload = GetGameInstance()
				? GetGameInstance()->GetSubsystem<UDSPGCSUploadJobSubsystem>()
				: nullptr)
			{
				const FDSPGCSMavFtpRuntimeConfigStatus Status =
					Upload->GetMavFtpRuntimeConfigStatus();
				if (!Status.bHasSnapshot && !Status.bRequestInFlight)
				{
					FString Ignored;
					Upload->RequestMavFtpRuntimeConfigSnapshot(Ignored);
				}
			}
			RefreshUploadDashboardUI();
		}
	}
}

void UGCSUIBridgeSubsystem::ResetUploadDashboard()
{
	UnbindUploadDashboardDataControls();
	UnbindUploadDashboardControls();
	if (UploadDashboardWidget)
	{
		UploadDashboardWidget->SetVisibility(ESlateVisibility::Collapsed);
		UploadDashboardWidget->RemoveFromParent();
		UploadDashboardWidget = nullptr;
	}
	bUploadDashboardVisible = false;
	bUploadDashboardDraftDirty = false;
	bUploadDashboardApplyPending = false;
	bUpdatingUploadDashboardDraft = false;
	UploadDashboardDraftGlobalCap = 0;
	UploadDashboardObservedConfigRevision = 0;
	UploadDashboardObservedAuthoritySessionID.Reset();
	UploadDashboardTuningStatus = TEXT("NOT CONNECTED");
	UploadDashboardTuningDiagnostic.Reset();
	UploadDashboardRefreshAccumulator = 0.0f;
#if WITH_DEV_AUTOMATION_TESTS
	UploadDashboardFactoryForTest = nullptr;
#endif
}

void UGCSUIBridgeSubsystem::BindUploadDashboardDataControls()
{
	UnbindUploadDashboardDataControls();
	UUserWidget* Dashboard = UploadDashboardWidget;
	if (!Dashboard || !Dashboard->WidgetTree)
	{
		return;
	}

	UploadDashboardApplyButton = FindDashboardWidget<UButton>(
		Dashboard, TEXT("BTN_ApplyMavFtpTuning"));
	UploadDashboardAutoCapButton = FindDashboardWidget<UButton>(
		Dashboard, TEXT("BTN_AutoGlobalWriteCap"));
	UploadDashboardRestoreButton = FindDashboardWidget<UButton>(
		Dashboard, TEXT("BTN_RestoreMavFtpBaseline"));
	UploadDashboardWriteWindowInput = Dashboard->WidgetTree->FindWidget(
		TEXT("EDT_MavFtpWriteWindow"));
	UploadDashboardGlobalCapInput = Dashboard->WidgetTree->FindWidget(
		TEXT("EDT_MavFtpGlobalWriteCap"));
	UploadDashboardSegmentItemsInput = Dashboard->WidgetTree->FindWidget(
		TEXT("EDT_MavFtpSegmentItems"));
	UploadDashboardRequestTimeoutInput = Dashboard->WidgetTree->FindWidget(
		TEXT("EDT_MavFtpRequestTimeoutMs"));
	UploadDashboardTerminateTimeoutInput = Dashboard->WidgetTree->FindWidget(
		TEXT("EDT_MavFtpTerminateTimeoutMs"));
	UploadDashboardRetryCountInput = Dashboard->WidgetTree->FindWidget(
		TEXT("EDT_MavFtpRetryCount"));

	if (UButton* Button = UploadDashboardApplyButton.Get())
	{
		Button->OnClicked.RemoveDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardApplyClicked);
		Button->OnClicked.AddDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardApplyClicked);
	}
	if (UButton* Button = UploadDashboardAutoCapButton.Get())
	{
		Button->OnClicked.RemoveDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardAutoCapClicked);
		Button->OnClicked.AddDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardAutoCapClicked);
	}
	if (UButton* Button = UploadDashboardRestoreButton.Get())
	{
		Button->OnClicked.RemoveDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardRestoreClicked);
		Button->OnClicked.AddDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardRestoreClicked);
	}
	if (UEditableText* Input = Cast<UEditableText>(
		UploadDashboardGlobalCapInput.Get()))
	{
		Input->OnTextChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleUploadDashboardGlobalCapTextChanged);
		Input->OnTextChanged.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleUploadDashboardGlobalCapTextChanged);
	}
	else if (UEditableTextBox* InputBox = Cast<UEditableTextBox>(
		UploadDashboardGlobalCapInput.Get()))
	{
		InputBox->OnTextChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleUploadDashboardGlobalCapTextChanged);
		InputBox->OnTextChanged.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleUploadDashboardGlobalCapTextChanged);
	}

	for (const TWeakObjectPtr<UWidget>& ReadOnly : {
		UploadDashboardWriteWindowInput,
		UploadDashboardSegmentItemsInput,
		UploadDashboardRequestTimeoutInput,
		UploadDashboardTerminateTimeoutInput,
		UploadDashboardRetryCountInput })
	{
		SetDashboardInputReadOnly(ReadOnly.Get(), true);
	}

	bUploadDashboardDataBound = UploadDashboardApplyButton.IsValid() &&
		UploadDashboardAutoCapButton.IsValid() &&
		UploadDashboardRestoreButton.IsValid() &&
		UploadDashboardWriteWindowInput.IsValid() &&
		UploadDashboardGlobalCapInput.IsValid() &&
		UploadDashboardSegmentItemsInput.IsValid() &&
		UploadDashboardRequestTimeoutInput.IsValid() &&
		UploadDashboardTerminateTimeoutInput.IsValid() &&
		UploadDashboardRetryCountInput.IsValid();
	if (!bUploadDashboardDataBound)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[UploadDashboard] One or more required C++ data controls are missing."));
	}
	RefreshUploadDashboardUI();
}

void UGCSUIBridgeSubsystem::UnbindUploadDashboardDataControls()
{
	if (UButton* Button = UploadDashboardApplyButton.Get())
	{
		Button->OnClicked.RemoveDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardApplyClicked);
	}
	if (UButton* Button = UploadDashboardAutoCapButton.Get())
	{
		Button->OnClicked.RemoveDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardAutoCapClicked);
	}
	if (UButton* Button = UploadDashboardRestoreButton.Get())
	{
		Button->OnClicked.RemoveDynamic(
			this, &UGCSUIBridgeSubsystem::HandleUploadDashboardRestoreClicked);
	}
	if (UEditableText* Input = Cast<UEditableText>(
		UploadDashboardGlobalCapInput.Get()))
	{
		Input->OnTextChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleUploadDashboardGlobalCapTextChanged);
	}
	else if (UEditableTextBox* InputBox = Cast<UEditableTextBox>(
		UploadDashboardGlobalCapInput.Get()))
	{
		InputBox->OnTextChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleUploadDashboardGlobalCapTextChanged);
	}
	UploadDashboardApplyButton.Reset();
	UploadDashboardAutoCapButton.Reset();
	UploadDashboardRestoreButton.Reset();
	UploadDashboardWriteWindowInput.Reset();
	UploadDashboardGlobalCapInput.Reset();
	UploadDashboardSegmentItemsInput.Reset();
	UploadDashboardRequestTimeoutInput.Reset();
	UploadDashboardTerminateTimeoutInput.Reset();
	UploadDashboardRetryCountInput.Reset();
	bUploadDashboardDataBound = false;
}

bool UGCSUIBridgeSubsystem::GetUploadDashboardTargetCount(
	int32& OutTargetCount) const
{
	OutTargetCount = 0;
	const UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return false;
	}
	if (const UDSPGCSUploadJobSubsystem* Upload =
		GameInstance->GetSubsystem<UDSPGCSUploadJobSubsystem>())
	{
		if (const dspgcs::UploadJobSnapshotDTO* Job =
			Upload->GetStateMirror().GetSnapshot())
		{
			if (Job->matchedCount > 0 &&
				Job->matchedCount <= static_cast<uint32>(MAX_int32))
			{
				OutTargetCount = static_cast<int32>(Job->matchedCount);
				return true;
			}
		}
	}
	if (const UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
		GameInstance->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>())
	{
		const FDSPGCSFrozenAssignmentSyncStatus Status = Sync->GetSyncStatus();
		if (Status.State == EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted &&
			Status.MatchedCount > 0)
		{
			OutTargetCount = Status.MatchedCount;
			return true;
		}
	}
	const int32 SelectedCount = GetSelectedCommandDroneIDs().Num();
	if (SelectedCount > 0)
	{
		OutTargetCount = SelectedCount;
		return true;
	}
	return false;
}

bool UGCSUIBridgeSubsystem::IsUploadDashboardMutationLocked() const
{
	if (RouteWorkflowCoordinator &&
		RouteWorkflowCoordinator->IsCommandSelectionMutationBlocked())
	{
		return true;
	}
	const UGameInstance* GameInstance = GetGameInstance();
	const UDSPGCSUploadJobSubsystem* Upload = GameInstance
		? GameInstance->GetSubsystem<UDSPGCSUploadJobSubsystem>() : nullptr;
	if (!Upload)
	{
		return false;
	}
	const FDSPGCSUploadJobCommandStatus Command = Upload->GetCommandStatus();
	if (Command.bCommandInFlight)
	{
		return true;
	}
	if (const dspgcs::UploadJobSnapshotDTO* Job =
		Upload->GetStateMirror().GetSnapshot())
	{
		return Job->jobState != dspgcs::UploadJobState::Completed &&
			Job->jobState != dspgcs::UploadJobState::Failed &&
			Job->jobState != dspgcs::UploadJobState::Cancelled;
	}
	return Command.bHasExecutableJob;
}

void UGCSUIBridgeSubsystem::SetUploadDashboardStatus(
	const FString& Status,
	const FString& Diagnostic)
{
	UploadDashboardTuningStatus = Status;
	UploadDashboardTuningDiagnostic = Diagnostic.Left(512);
	SetDashboardText(
		UploadDashboardWidget,
		TEXT("TXT_MavFtpTuningStatus"),
		UploadDashboardTuningStatus,
		UploadDashboardTuningDiagnostic);
}

void UGCSUIBridgeSubsystem::TickUploadDashboardUI(const float DeltaSeconds)
{
	if (!bUploadDashboardVisible || !UploadDashboardWidget)
	{
		UploadDashboardRefreshAccumulator = 0.0f;
		return;
	}
	UploadDashboardRefreshAccumulator += FMath::Max(0.0f, DeltaSeconds);
	if (UploadDashboardRefreshAccumulator < UploadDashboardRefreshIntervalSeconds)
	{
		return;
	}
	UploadDashboardRefreshAccumulator = 0.0f;
	RefreshUploadDashboardUI();
}

void UGCSUIBridgeSubsystem::RefreshUploadDashboardUI()
{
	UUserWidget* Dashboard = UploadDashboardWidget;
	UGameInstance* GameInstance = GetGameInstance();
	if (!Dashboard || !GameInstance)
	{
		return;
	}

	UDSPGCSUploadJobSubsystem* Upload =
		GameInstance->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	const FDSPGCSMavFtpRuntimeConfigStatus ConfigStatus = Upload
		? Upload->GetMavFtpRuntimeConfigStatus()
		: FDSPGCSMavFtpRuntimeConfigStatus{};
	const dspgcs::MavFtpRuntimeConfigSnapshotDTO* Config = Upload
		? Upload->GetMavFtpRuntimeConfigSnapshot() : nullptr;
	const TCHAR* HeaderStatus = TEXT("LOADING");
	switch (ConfigStatus.State)
	{
	case EDSPGCSMavFtpRuntimeConfigState::Connected:
		HeaderStatus = Config ? TEXT("CONNECTED") : TEXT("LOADING");
		break;
	case EDSPGCSMavFtpRuntimeConfigState::Disconnected:
		HeaderStatus = TEXT("DISCONNECTED");
		break;
	case EDSPGCSMavFtpRuntimeConfigState::Stale:
		HeaderStatus = TEXT("STALE");
		break;
	default:
		break;
	}
	SetDashboardText(Dashboard, TEXT("TXT_Status"), HeaderStatus,
		ConfigStatus.LastSafeDiagnostic);

	const bool bSessionChanged = Config &&
		!UploadDashboardObservedAuthoritySessionID.IsEmpty() &&
		UploadDashboardObservedAuthoritySessionID !=
			FString(UTF8_TO_TCHAR(Config->authoritySessionId.c_str()));
	const bool bRevisionChanged = Config &&
		UploadDashboardObservedConfigRevision != Config->configRevision;
	if (Config && (UploadDashboardObservedAuthoritySessionID.IsEmpty() ||
		bSessionChanged || bRevisionChanged))
	{
		if (!bUploadDashboardDraftDirty && !bUploadDashboardApplyPending)
		{
			UploadDashboardDraftGlobalCap = static_cast<int32>(
				Config->runtimeAcceptedGlobalWriteCap);
		}
		else if (bSessionChanged || bRevisionChanged)
		{
			SetUploadDashboardStatus(
				TEXT("STALE"),
				TEXT("Console Runtime Config changed while the local Draft was modified."));
		}
		UploadDashboardObservedAuthoritySessionID =
			UTF8_TO_TCHAR(Config->authoritySessionId.c_str());
		UploadDashboardObservedConfigRevision = Config->configRevision;
	}

	if (bUploadDashboardApplyPending && !ConfigStatus.bRequestInFlight)
	{
		bUploadDashboardApplyPending = false;
		if (ConfigStatus.bLastApplyAccepted && Config)
		{
			UploadDashboardDraftGlobalCap = static_cast<int32>(
				Config->runtimeAcceptedGlobalWriteCap);
			bUploadDashboardDraftDirty = false;
			SetUploadDashboardStatus(
				TEXT("APPLIED"), ConfigStatus.LastSafeDiagnostic);
		}
		else
		{
			SetUploadDashboardStatus(
				TEXT("REJECTED"), ConfigStatus.LastSafeDiagnostic);
		}
	}
	else if (!Config)
	{
		SetUploadDashboardStatus(TEXT("NOT CONNECTED"),
			ConfigStatus.LastSafeDiagnostic);
	}
	else if (IsUploadDashboardMutationLocked())
	{
		SetUploadDashboardStatus(TEXT("JOB LOCKED"));
	}
	else if (bUploadDashboardApplyPending)
	{
		SetUploadDashboardStatus(TEXT("APPLYING"));
	}
	else if (bUploadDashboardDraftDirty &&
		UploadDashboardTuningStatus != TEXT("STALE") &&
		UploadDashboardTuningStatus != TEXT("REJECTED"))
	{
		SetUploadDashboardStatus(TEXT("MODIFIED"));
	}
	else if (!bUploadDashboardDraftDirty &&
		UploadDashboardTuningStatus != TEXT("APPLIED"))
	{
		SetUploadDashboardStatus(TEXT("CURRENT"));
	}

	bUpdatingUploadDashboardDraft = true;
	if (Config)
	{
		SetDashboardInputText(UploadDashboardWriteWindowInput.Get(),
			FString::FromInt(Config->writeWindow));
		SetDashboardInputText(UploadDashboardSegmentItemsInput.Get(),
			FString::FromInt(Config->segmentItems));
		SetDashboardInputText(UploadDashboardRequestTimeoutInput.Get(),
			FString::FromInt(Config->requestTimeoutMs));
		SetDashboardInputText(UploadDashboardTerminateTimeoutInput.Get(),
			FString::FromInt(Config->terminateTimeoutMs));
		SetDashboardInputText(UploadDashboardRetryCountInput.Get(),
			FString::FromInt(Config->retryCount));
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpWritePayloadValue"),
			FString::Printf(TEXT("%u bytes"), Config->writePayload));
	}
	else
	{
		for (const TWeakObjectPtr<UWidget>& InputRef : {
			UploadDashboardWriteWindowInput,
			UploadDashboardSegmentItemsInput,
			UploadDashboardRequestTimeoutInput,
			UploadDashboardTerminateTimeoutInput,
			UploadDashboardRetryCountInput })
		{
			SetDashboardInputText(InputRef.Get(), TEXT("-"));
		}
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpWritePayloadValue"), TEXT("-"));
	}
	if (UWidget* Input = UploadDashboardGlobalCapInput.Get())
	{
		if (!bUploadDashboardDraftDirty && Config)
			UploadDashboardDraftGlobalCap = static_cast<int32>(
				Config->runtimeAcceptedGlobalWriteCap);
		if (UploadDashboardDraftGlobalCap > 0)
			SetDashboardInputText(Input,
				FString::FromInt(UploadDashboardDraftGlobalCap));
		SetDashboardInputReadOnly(Input,
			!Config || IsUploadDashboardMutationLocked() ||
			bUploadDashboardApplyPending);
	}
	bUpdatingUploadDashboardDraft = false;

	const URouteArtifactWorkspaceSubsystem* Workspace =
		GameInstance->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	const FRouteArtifactSourceIdentity* Source = Workspace
		? Workspace->GetCurrentSourceIdentity() : nullptr;
	SetDashboardText(Dashboard, TEXT("TXT_MissionFile"),
		Source ? Source->SourceDisplayFileName : TEXT("-"));

	int32 TargetCount = 0;
	const bool bHasTargets = GetUploadDashboardTargetCount(TargetCount);
	SetDashboardText(Dashboard, TEXT("TXT_TotalDroneCount"),
		bHasTargets ? FString::FromInt(TargetCount) : TEXT("-"));
	const dspgcs::UploadJobSnapshotDTO* Job = Upload
		? Upload->GetStateMirror().GetSnapshot() : nullptr;
	SetDashboardText(Dashboard, TEXT("TXT_TotalItems"),
		Job && Job->artifactMissionItemsTotal > 0
			? FString::Printf(TEXT("%llu"),
				static_cast<unsigned long long>(Job->artifactMissionItemsTotal))
			: TEXT("-"));
	SetDashboardText(Dashboard, TEXT("TXT_TotalSegments"),
		Job && Job->plannedSegmentCountTotal > 0
			? FString::Printf(TEXT("%llu"),
				static_cast<unsigned long long>(Job->plannedSegmentCountTotal))
			: TEXT("-"));

	const int32 AppliedConcurrency = RouteWorkflowCoordinator
		? RouteWorkflowCoordinator->GetAppliedMaxConcurrentTargets()
		: UGCSRouteWorkflowCoordinator::DefaultMaxConcurrentTargets;
	SetDashboardText(Dashboard, TEXT("TXT_AnalysisMaxConcurrencyValue"),
		FString::FromInt(AppliedConcurrency));
	if (Config && bHasTargets)
	{
		const int32 EffectiveTargets = FMath::Min(AppliedConcurrency, TargetCount);
		const int32 RequiredCap = FMath::Min(
			EffectiveTargets * static_cast<int32>(Config->writeWindow), 800);
		const bool bFullWindow =
			static_cast<int32>(Config->runtimeAcceptedGlobalWriteCap) >= RequiredCap;
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpRequiredGlobalCap"),
			FString::FromInt(RequiredCap));
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpAppliedGlobalCap"),
			FString::FromInt(Config->runtimeAcceptedGlobalWriteCap));
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpFullWindowStatus"),
			bFullWindow ? TEXT("YES") : TEXT("NO"));
	}
	else
	{
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpRequiredGlobalCap"), TEXT("-"));
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpAppliedGlobalCap"),
			Config ? FString::FromInt(Config->runtimeAcceptedGlobalWriteCap) : TEXT("-"));
		SetDashboardText(Dashboard, TEXT("TXT_MavFtpFullWindowStatus"), TEXT("-"));
	}

	const bool bCanMutate = Config &&
		ConfigStatus.State == EDSPGCSMavFtpRuntimeConfigState::Connected &&
		!ConfigStatus.bRequestInFlight && !IsUploadDashboardMutationLocked();
	if (UButton* Button = UploadDashboardApplyButton.Get())
		Button->SetIsEnabled(bCanMutate && bUploadDashboardDraftDirty);
	if (UButton* Button = UploadDashboardAutoCapButton.Get())
		Button->SetIsEnabled(bCanMutate && bHasTargets);
	if (UButton* Button = UploadDashboardRestoreButton.Get())
		Button->SetIsEnabled(bCanMutate);
}

void UGCSUIBridgeSubsystem::HandleUploadDashboardGlobalCapTextChanged(
	const FText& Text)
{
	if (bUpdatingUploadDashboardDraft)
	{
		return;
	}
	int32 Parsed = 0;
	if (ParseDashboardGlobalCap(Text, Parsed))
	{
		UploadDashboardDraftGlobalCap = Parsed;
		bUploadDashboardDraftDirty = true;
		SetUploadDashboardStatus(TEXT("MODIFIED"));
	}
	else
	{
		bUploadDashboardDraftDirty = true;
		SetUploadDashboardStatus(
			TEXT("REJECTED"), TEXT("Global Write Cap must be an integer in range 1..800."));
	}
	RefreshUploadDashboardUI();
}

void UGCSUIBridgeSubsystem::HandleUploadDashboardApplyClicked()
{
	if (bUploadDashboardApplyPending || IsUploadDashboardMutationLocked())
	{
		return;
	}
	int32 Candidate = 0;
	if (!UploadDashboardGlobalCapInput.IsValid() ||
		!ParseDashboardGlobalCap(
			GetDashboardInputText(UploadDashboardGlobalCapInput.Get()), Candidate))
	{
		SetUploadDashboardStatus(
			TEXT("REJECTED"), TEXT("Global Write Cap must be an integer in range 1..800."));
		RefreshUploadDashboardUI();
		return;
	}
	UDSPGCSUploadJobSubsystem* Upload = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UDSPGCSUploadJobSubsystem>() : nullptr;
	FString Error;
	if (!Upload || !Upload->ApplyMavFtpRuntimeGlobalCap(Candidate, Error))
	{
		SetUploadDashboardStatus(TEXT("REJECTED"), Error);
		RefreshUploadDashboardUI();
		return;
	}
	UploadDashboardDraftGlobalCap = Candidate;
	bUploadDashboardApplyPending = true;
	SetUploadDashboardStatus(TEXT("APPLYING"));
	RefreshUploadDashboardUI();
}

void UGCSUIBridgeSubsystem::HandleUploadDashboardAutoCapClicked()
{
	const UDSPGCSUploadJobSubsystem* Upload = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UDSPGCSUploadJobSubsystem>() : nullptr;
	const auto* Config = Upload ? Upload->GetMavFtpRuntimeConfigSnapshot() : nullptr;
	int32 TargetCount = 0;
	if (!Config || !GetUploadDashboardTargetCount(TargetCount))
	{
		SetUploadDashboardStatus(
			TEXT("REJECTED"), TEXT("Auto Cap requires a current Config and Target set."));
		return;
	}
	const int32 AppliedConcurrency = RouteWorkflowCoordinator
		? RouteWorkflowCoordinator->GetAppliedMaxConcurrentTargets()
		: UGCSRouteWorkflowCoordinator::DefaultMaxConcurrentTargets;
	UploadDashboardDraftGlobalCap = FMath::Min(
		FMath::Min(AppliedConcurrency, TargetCount) *
			static_cast<int32>(Config->writeWindow), 800);
	bUploadDashboardDraftDirty = true;
	SetUploadDashboardStatus(TEXT("MODIFIED"));
	RefreshUploadDashboardUI();
}

void UGCSUIBridgeSubsystem::HandleUploadDashboardRestoreClicked()
{
	const UDSPGCSUploadJobSubsystem* Upload = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UDSPGCSUploadJobSubsystem>() : nullptr;
	const auto* Config = Upload ? Upload->GetMavFtpRuntimeConfigSnapshot() : nullptr;
	if (!Config)
	{
		SetUploadDashboardStatus(TEXT("NOT CONNECTED"));
		return;
	}
	UploadDashboardDraftGlobalCap = static_cast<int32>(
		Config->bootDefaultGlobalWriteCap);
	bUploadDashboardDraftDirty = true;
	SetUploadDashboardStatus(TEXT("MODIFIED"));
	RefreshUploadDashboardUI();
}

#if WITH_DEV_AUTOMATION_TESTS
void UGCSUIBridgeSubsystem::BindUploadDashboardForTest(
	UUserWidget* MainWidget,
	UUserWidget* RightToolbar)
{
	UnbindUploadDashboardControls();
	MainWidgetRef = MainWidget;
	RightToolbarWidget = RightToolbar;
	BindUploadDashboardControls();
}

void UGCSUIBridgeSubsystem::SetUploadDashboardFactoryForTest(
	TFunction<UUserWidget*()> Factory)
{
	UploadDashboardFactoryForTest = MoveTemp(Factory);
}

bool UGCSUIBridgeSubsystem::IsUploadDashboardToggleBoundForTest() const
{
	const UButton* Button = UploadDashboardToggleButton.Get();
	return Button && Button->OnClicked.IsAlreadyBound(
		this,
		&UGCSUIBridgeSubsystem::HandleUploadDashboardToggleClicked);
}
#endif

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
	bFleetPanelRequestedVisible = !bFleetPanelRequestedVisible;
	ApplyNavigationVisibility();
}

void UGCSUIBridgeSubsystem::HandleControlPageClicked()
{
	bDroneDetailRequestedVisible = !bDroneDetailRequestedVisible;
	ApplyNavigationVisibility();
}

void UGCSUIBridgeSubsystem::HandleMapPageClicked()
{
	if (UUserWidget* Page = MapPageWidget.Get())
	{
		const ESlateVisibility Visibility = Page->GetVisibility();
		const bool bIsShown = Visibility != ESlateVisibility::Collapsed
			&& Visibility != ESlateVisibility::Hidden;
		FGCSUIWidgetPresenter::SetWidgetVisible(
			MapPageWidget,
			!bIsShown,
			true);
	}
}

void UGCSUIBridgeSubsystem::HandleSettingsPageClicked()
{
	if (UUserWidget* Playback = PlaybackWidget.Get())
	{
		const ESlateVisibility Visibility = Playback->GetVisibility();
		const bool bIsShown = Visibility != ESlateVisibility::Collapsed
			&& Visibility != ESlateVisibility::Hidden;
		FGCSUIWidgetPresenter::SetWidgetVisible(
			PlaybackWidget,
			!bIsShown);
	}
}

void UGCSUIBridgeSubsystem::HandleLogsPageClicked()
{
	if (UUserWidget* Page = LogsPageWidget.Get())
	{
		const ESlateVisibility Visibility = Page->GetVisibility();
		const bool bIsShown = Visibility != ESlateVisibility::Collapsed
			&& Visibility != ESlateVisibility::Hidden;
		FGCSUIWidgetPresenter::SetWidgetVisible(
			LogsPageWidget,
			!bIsShown,
			true);
	}
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

void UGCSUIBridgeSubsystem::HandleTimeStartClicked()
{
	if (bTimeControlCountdownActive || bTimeControlPerformanceActive
		|| !PendingTimeControlCommandID.IsEmpty())
	{
		SetRightPanelText(
			TEXT("TXT_TimeControlStatus"),
			PendingTimeControlCommandType == EGCSProductionCommandType::TimeStop
				? TEXT("正在取消授时并发送降落，请等待完成")
				: TEXT("已有授时任务，请先结束当前任务，勿重复授时"));
		return;
	}
	USpinBox* HourInput = FindRightPanelSpinBox(TEXT("SPN_TimeHour"));
	USpinBox* MinuteInput = FindRightPanelSpinBox(TEXT("SPN_TimeMinute"));
	USpinBox* SecondInput = FindRightPanelSpinBox(TEXT("SPN_TimeSecond"));
	const TArray<FString> Targets = GetCommandTargetDroneIDs();
	if (!HourInput || !MinuteInput || !SecondInput)
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("授时控件缺失，请检查资产命名"));
		return;
	}
	if (Targets.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("请先选择无人机"));
		return;
	}

	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview || !RoutePreview->IsRouteLoaded())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("请先导入本次表演的NPZ航线"));
		return;
	}
	FString SampleRateError;
	if (!RoutePreview->SetSampleRateHz(URoutePreviewSubsystem::ScheduledShowSampleRateHz, SampleRateError))
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("设置表演帧率失败，未发送授时"));
		UE_LOG(LogTemp, Warning, TEXT("[TimeControlUI] START blocked before dispatch: %s"), *SampleRateError);
		return;
	}

	const int32 Hour = FMath::Clamp(FMath::RoundToInt(HourInput->GetValue()), 0, 23);
	const int32 Minute = FMath::Clamp(FMath::RoundToInt(MinuteInput->GetValue()), 0, 59);
	const int32 Second = FMath::Clamp(FMath::RoundToInt(SecondInput->GetValue()), 0, 59);
	const FDateTime Now = FDateTime::Now();
	FDateTime RequestedTimeLocal(
		Now.GetYear(),
		Now.GetMonth(),
		Now.GetDay(),
		Hour,
		Minute,
		Second);
	if (RequestedTimeLocal <= Now)
	{
		RequestedTimeLocal += FTimespan::FromDays(1.0);
	}
	const double LeadSeconds = (RequestedTimeLocal - Now).GetTotalSeconds();

	ResetTimeControlPreparationState();
	ScheduledTimeLocal = RequestedTimeLocal;
	TimeControlPreviewDatasetGeneration = RoutePreview->GetRouteDatasetGeneration();
	SetRightPanelText(TEXT("TXT_TimeCountdownLabel"), TEXT("倒计时"));
	SetRightPanelText(
		TEXT("TXT_TimeCountdown"),
		FormatTimeControlDuration(FMath::CeilToInt(LeadSeconds)));
	TimeControlTargetDroneIDs = Targets;
	TArray<FString> OnlineTargets;
	if (const UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>())
	{
		for (const FString& DroneID : Targets)
		{
			FDroneData Drone;
			if (Fleet->GetDroneByID(DroneID, Drone) && Drone.bOnline)
			{
				OnlineTargets.Add(DroneID);
			}
			else
			{
				TimeControlFailedDroneIDs.Add(DroneID);
			}
		}
	}
	if (OnlineTargets.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeCountdown"), TEXT("--:--:--"));
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("授时失败：所选无人机均不在线"));
		return;
	}

	FGCSProductionCommandParameters Parameters;
	Parameters.TimeControlMask = 1;
	Parameters.ScheduledHour = static_cast<uint8>(Hour);
	Parameters.ScheduledMinute = static_cast<uint8>(Minute);
	Parameters.ScheduledSecond = static_cast<uint8>(Second);
	const FString CommandID = SubmitProductionCommandForTargets(
		EGCSProductionCommandType::TimeStart,
		OnlineTargets,
		Parameters);
	if (CommandID.IsEmpty())
	{
		bTimeControlCountdownActive = false;
		SetRightPanelText(TEXT("TXT_TimeCountdown"), TEXT("--:--:--"));
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("授时飞行发送失败"));
		return;
	}

	PendingTimeControlCommandID = CommandID;
	PendingTimeControlCommandType = EGCSProductionCommandType::TimeStart;
	PendingTimeControlTargetCount = OnlineTargets.Num();
	PendingTimeControlResultCount = 0;
	PendingTimeControlSucceededCount = 0;
	PendingTimeControlCompletedDroneIDs.Reset();
	bTimeControlCountdownActive = true;
	TimeControlUIRefreshAccumulator = TimeControlUIRefreshIntervalSeconds;
	SetRightPanelText(
		TEXT("TXT_TimeControlStatus"),
		FString::Printf(
			TEXT("正在发送授时飞行：0/%d  %02d:%02d:%02d"),
			OnlineTargets.Num(),
			Hour,
			Minute,
			Second));
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[TimeControlUI] START CommandID=%s Targets=%d LocalTime=%02d:%02d:%02d"),
		*CommandID,
		OnlineTargets.Num(),
		Hour,
		Minute,
		Second);
}

void UGCSUIBridgeSubsystem::HandleTimeStopClicked()
{
	if (!PendingTimeControlCommandID.IsEmpty()
		&& PendingTimeControlCommandType == EGCSProductionCommandType::TimeStop)
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("正在取消授时并发送降落，请勿重复操作"));
		return;
	}

	const TArray<FString> Targets = TimeControlTargetDroneIDs.IsEmpty()
		? GetCommandTargetDroneIDs()
		: TimeControlTargetDroneIDs;
	if (Targets.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("请先选择无人机"));
		return;
	}

	FGCSProductionCommandParameters Parameters;
	Parameters.TimeControlMask = 1;
	const FString CommandID = SubmitProductionCommandForTargets(
		EGCSProductionCommandType::TimeStop,
		Targets,
		Parameters);
	if (CommandID.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("授时结束发送失败"));
		return;
	}

	bTimeControlCountdownActive = false;
	const bool bWasPerformanceActive = bTimeControlPerformanceActive;
	bTimeControlPerformanceActive = false;
	SetRightPanelText(TEXT("TXT_TimeCountdownLabel"), TEXT("倒计时"));
	SetRightPanelText(TEXT("TXT_TimeCountdown"), TEXT("--:--:--"));
	if (URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>())
	{
		if (bWasPerformanceActive)
		{
			RoutePreview->Pause();
		}
	}
	PendingTimeControlCommandID = CommandID;
	PendingTimeControlCommandType = EGCSProductionCommandType::TimeStop;
	PendingTimeControlTargetCount = Targets.Num();
	PendingTimeControlResultCount = 0;
	PendingTimeControlSucceededCount = 0;
	PendingTimeControlCompletedDroneIDs.Reset();
	SetRightPanelText(
		TEXT("TXT_TimeControlStatus"),
		FString::Printf(TEXT("正在取消授时并发送降落：0/%d"), Targets.Num()));
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[TimeControlUI] STOP CommandID=%s Targets=%d"),
		*CommandID,
		Targets.Num());
}

void UGCSUIBridgeSubsystem::HandleExecuteDanceStepClicked()
{
	const TArray<FString> Targets = GetCommandTargetDroneIDs();
	if (Targets.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("请先选择无人机"));
		return;
	}

	const FString CommandID = SubmitProductionCommandForTargets(
		EGCSProductionCommandType::ExecuteDanceStep,
		Targets);
	if (CommandID.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("执行舞步发送失败"));
		return;
	}

	// The physical command must not depend on local NPZ state. If a matching
	// route is loaded, start the UE visualization from frame zero as well.
	if (URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
		RoutePreview && RoutePreview->IsRouteLoaded())
	{
		ResetTimeControlPreparationState();
		TimeControlTargetDroneIDs = Targets;
		TimeControlPreviewDatasetGeneration = RoutePreview->GetRouteDatasetGeneration();
		FString PreviewError;
		if (!BeginScheduledRoutePreview(PreviewError))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[TimeControlUI] EXECUTE local preview rejected: %s"),
				*PreviewError);
		}
	}

	SetRightPanelText(
		TEXT("TXT_TimeControlStatus"),
		FString::Printf(TEXT("执行舞步已发送：AUTO＋起飞（%d架）"), Targets.Num()));
}

void UGCSUIBridgeSubsystem::HandleEndDanceStepClicked()
{
	const TArray<FString> Targets = TimeControlTargetDroneIDs.IsEmpty()
		? GetCommandTargetDroneIDs()
		: TimeControlTargetDroneIDs;
	if (Targets.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("请先选择无人机"));
		return;
	}

	FGCSProductionCommandParameters Parameters;
	Parameters.TimeControlMask = 1;
	const FString CommandID = SubmitProductionCommandForTargets(
		EGCSProductionCommandType::EndDanceStep,
		Targets,
		Parameters);
	if (CommandID.IsEmpty())
	{
		SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("结束舞步发送失败"));
		return;
	}

	if (URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>())
	{
		FString PreviewError;
		RoutePreview->Stop(PreviewError);
	}
	ResetTimeControlPreparationState();
	SetRightPanelText(TEXT("TXT_TimeCountdownLabel"), TEXT("倒计时"));
	SetRightPanelText(TEXT("TXT_TimeCountdown"), TEXT("--:--:--"));
	SetRightPanelText(
		TEXT("TXT_TimeControlStatus"),
		FString::Printf(TEXT("结束舞步已发送：取消授时＋降落（%d架）"), Targets.Num()));
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
	const auto IsSuccessfulResult = [](const FGCSProductionCommandResult& Value)
	{
		return Value.AckState.Equals(TEXT("SENT"), ESearchCase::IgnoreCase)
			|| Value.AckState.Equals(TEXT("ACCEPTED"), ESearchCase::IgnoreCase)
			|| Value.AckState.Equals(TEXT("SIMULATED"), ESearchCase::IgnoreCase);
	};
	const FString StateText =
		Result.CommandType == EGCSProductionCommandType::HoverTest && Result.bSimulated
			? TEXT("WAITING_COMMAND")
			: Result.AckState;
	const FString Diagnostic = !Result.Detail.IsEmpty()
		? Result.Detail
		: Result.ErrorCode;
	LastCommandResultText = FString::Printf(
		TEXT("CMD %s  %s%s%s  %s"),
		*Result.DroneID,
		*StateText,
		Diagnostic.IsEmpty() ? TEXT("") : TEXT("  "),
		*Diagnostic,
		*Result.CommandID);
	SetText(TEXT("Attitude"), LastCommandResultText);

	if (!PendingTimeControlCommandID.IsEmpty()
		&& Result.CommandID == PendingTimeControlCommandID
		&& Result.bFinal
		&& !PendingTimeControlCompletedDroneIDs.Contains(Result.DroneID))
	{
		PendingTimeControlCompletedDroneIDs.Add(Result.DroneID);
		++PendingTimeControlResultCount;
		const bool bSucceeded = IsSuccessfulResult(Result);
		if (bSucceeded)
		{
			++PendingTimeControlSucceededCount;
			if (PendingTimeControlCommandType == EGCSProductionCommandType::TimeStart)
			{
				TimeControlScheduleSentDroneIDs.Add(Result.DroneID);
			}
		}
		else if (PendingTimeControlCommandType == EGCSProductionCommandType::TimeStart)
		{
			TimeControlFailedDroneIDs.Add(Result.DroneID);
		}

		if (PendingTimeControlResultCount >= PendingTimeControlTargetCount)
		{
			const bool bStart = PendingTimeControlCommandType ==
				EGCSProductionCommandType::TimeStart;
			if (bStart)
			{
				if (PendingTimeControlSucceededCount == 0)
				{
					bTimeControlCountdownActive = false;
					SetRightPanelText(TEXT("TXT_TimeCountdownLabel"), TEXT("倒计时"));
					SetRightPanelText(TEXT("TXT_TimeCountdown"), TEXT("--:--:--"));
					SetRightPanelText(
						TEXT("TXT_TimeControlStatus"),
						FString::Printf(
							TEXT("授时飞行发送失败 0/%d，任务未建立"),
							PendingTimeControlTargetCount));
				}
				else
				{
					RefreshTimeControlPreparationStatus(TEXT("授时已发送（飞控未确认）"));
				}
			}
			else
			{
				SetRightPanelText(
					TEXT("TXT_TimeControlStatus"),
					FString::Printf(
						TEXT("授时已取消并发送降落 %d/%d"),
						PendingTimeControlSucceededCount,
						PendingTimeControlTargetCount));
			}
			PendingTimeControlCommandID.Reset();
			if (!bStart)
			{
				ResetTimeControlPreparationState();
			}
		}
		else
		{
			SetRightPanelText(
				TEXT("TXT_TimeControlStatus"),
				FString::Printf(
					TEXT("%s：%d/%d"),
					PendingTimeControlCommandType == EGCSProductionCommandType::TimeStart
						? TEXT("正在发送授时飞行")
						: TEXT("正在取消授时并发送降落"),
					PendingTimeControlResultCount,
					PendingTimeControlTargetCount));
		}
	}

	if (Result.bFinal &&
		(Result.CommandType == EGCSProductionCommandType::ExecuteDanceStep ||
		 Result.CommandType == EGCSProductionCommandType::EndDanceStep))
	{
		const TCHAR* ActionName = Result.CommandType ==
			EGCSProductionCommandType::ExecuteDanceStep
			? TEXT("执行舞步")
			: TEXT("结束舞步");
		SetRightPanelText(
			TEXT("TXT_TimeControlStatus"),
			FString::Printf(
				TEXT("%s %s：%s"),
				ActionName,
				*Result.DroneID,
				IsSuccessfulResult(Result) ? TEXT("已发送") : TEXT("失败")));
	}
	UE_LOG(
		LogTemp,
		Display,
		TEXT("DSP-GCS UI RESULT CommandID=%s Source=%s DroneID=%s Result=%s Error=%s Detail=%s"),
		*Result.CommandID,
		*Result.Source,
		*Result.DroneID,
		*Result.AckState,
		*Result.ErrorCode,
		*Result.Detail);
}

FString UGCSUIBridgeSubsystem::SubmitSelectedProductionCommand(
	const EGCSProductionCommandType CommandType,
	const FGCSProductionCommandParameters& Parameters)
{
	const TArray<FString> Targets = GetCommandTargetDroneIDs();
	if (Targets.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("DSP-GCS UI command rejected: no checked DroneID."));
		return {};
	}
	return SubmitProductionCommandForTargets(CommandType, Targets, Parameters);
}


FString UGCSUIBridgeSubsystem::SubmitProductionCommandForTargets(
	const EGCSProductionCommandType CommandType,
	const TArray<FString>& TargetDroneIDs,
	const FGCSProductionCommandParameters& Parameters)
{
	UDSPGCSProductionCommandProviderSubsystem* Commands =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>();
	if (!Commands || TargetDroneIDs.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("DSP-GCS UI command rejected: provider/targets invalid."));
		return {};
	}
	return Commands->SubmitCommand(
		CommandType,
		TargetDroneIDs,
		Parameters);
}

void UGCSUIBridgeSubsystem::HandleHoverPanelToggleClicked()
{
	if (UWidget* Content = HoverTestContentWidget.Get())
	{
		Content->SetVisibility(
			Content->GetVisibility() == ESlateVisibility::Collapsed
				? ESlateVisibility::Visible
				: ESlateVisibility::Collapsed);
	}
}

void UGCSUIBridgeSubsystem::HandleHoverGenerateLayersClicked()
{
	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (!Fleet)
	{
		ResetHoverLayerPlan(TEXT("无法读取机群数据。"));
		return;
	}
	if (IsCommandSelectionMutationBlocked())
	{
		ResetHoverLayerPlan(TEXT("当前任务已锁定选择，暂时不能生成分层。"));
		return;
	}

	TArray<FString> ScopeDroneIDs = GetCommandTargetDroneIDs();
	const bool bUsingCurrentSelection = !ScopeDroneIDs.IsEmpty();
	if (!bUsingCurrentSelection)
	{
		for (const FDroneData& Drone : Fleet->GetDronePage(0, Fleet->GetDroneCount()))
		{
			if (Drone.bOnline)
			{
				ScopeDroneIDs.Add(Drone.DroneID);
			}
		}
	}

	TArray<FHoverLayerPosition> Positions;
	FString Error;
	FString PositionSource;
	if (!BuildHoverPositions(ScopeDroneIDs, Positions, Error, &PositionSource))
	{
		ResetHoverLayerPlan(Error);
		return;
	}

	FHoverLayerPlan Plan;
	if (!HoverLayerSelection::BuildPlan(
		Positions,
		HoverLayerMinimumSpacingMeters,
		Plan,
		Error))
	{
		ResetHoverLayerPlan(Error);
		return;
	}

	HoverLayerBaseDroneIDs = MoveTemp(Plan.BaseDroneIDs);
	HoverLayers = MoveTemp(Plan.Layers);
	HoverLayerFleetSessionID = Fleet->GetActiveFleetSessionID();
	HoverLayerPositionSource = MoveTemp(PositionSource);
	HoverLayerOptions.Reset();
	HoverLayerOptions.Add(FString::Printf(
		TEXT("全部（%d架，仅选择）"),
		HoverLayerBaseDroneIDs.Num()));
	for (int32 Index = 0; Index < HoverLayers.Num(); ++Index)
	{
		HoverLayerOptions.Add(FString::Printf(
			TEXT("第%d层（%d架）"),
			Index + 1,
			HoverLayers[Index].Num()));
	}

	if (UComboBoxString* Combo = HoverLayerCombo.Get())
	{
		bUpdatingHoverLayerCombo = true;
		Combo->ClearOptions();
		for (const FString& Option : HoverLayerOptions)
		{
			Combo->AddOption(Option);
		}
		bUpdatingHoverLayerCombo = false;
	}

	HoverTestStatus = bUsingCurrentSelection
		? FString::Printf(
			TEXT("已按当前选择生成分层（%s）。"),
			*HoverLayerPositionSource)
		: FString::Printf(
			TEXT("未选择目标，已按全部在线无人机生成分层（%s）。"),
			*HoverLayerPositionSource);
	ApplyHoverLayerSelection(1);
}

void UGCSUIBridgeSubsystem::HandleHoverLayerSelectionChanged(
	const FString SelectedItem,
	const ESelectInfo::Type SelectionType)
{
	(void)SelectionType;
	if (bUpdatingHoverLayerCombo)
	{
		return;
	}
	ApplyHoverLayerSelection(HoverLayerOptions.IndexOfByKey(SelectedItem));
}

void UGCSUIBridgeSubsystem::HandleHoverAltitudeCommitted(
	const FText& Text,
	const ETextCommit::Type CommitMethod)
{
	(void)Text;
	(void)CommitMethod;
	FString Error;
	if (!TryReadHoverAltitude(Error))
	{
		HoverTestStatus = Error;
	}
	RefreshHoverTestControls();
}

void UGCSUIBridgeSubsystem::HandleHoverUnlockClicked()
{
	FString Error;
	if (PrepareHoverLayerAction(Error, false))
	{
		HandleArmClicked();
	}
	else
	{
		HoverTestStatus = Error;
		RefreshHoverTestControls();
	}
}

void UGCSUIBridgeSubsystem::HandleHoverTakeoffClicked()
{
	FString Error;
	if (TryReadHoverAltitude(Error) && PrepareHoverLayerAction(Error, true))
	{
		HandleHoverClicked();
	}
	else
	{
		HoverTestStatus = Error;
		RefreshHoverTestControls();
	}
}

void UGCSUIBridgeSubsystem::HandleHoverLandClicked()
{
	FString Error;
	if (PrepareHoverLayerAction(Error, false))
	{
		HandleLandClicked();
	}
	else
	{
		HoverTestStatus = Error;
		RefreshHoverTestControls();
	}
}

void UGCSUIBridgeSubsystem::HandleHoverLockClicked()
{
	FString Error;
	if (PrepareHoverLayerAction(Error, false))
	{
		HandleDisarmClicked();
	}
	else
	{
		HoverTestStatus = Error;
		RefreshHoverTestControls();
	}
}

void UGCSUIBridgeSubsystem::HandleHoverManualClicked()
{
	FString Error;
	if (PrepareHoverLayerAction(Error, false))
	{
		HandleManualModeClicked();
	}
	else
	{
		HoverTestStatus = Error;
		RefreshHoverTestControls();
	}
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
}

void UGCSUIBridgeSubsystem::HandleFleetSelectAllClicked()
{
	if (IsCommandSelectionMutationBlocked())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GCS_FLEET_SELECT_ALL] Rejected: active AutoStart/Upload Job."));
		RefreshFleetSelectAll();
		return;
	}

	const TArray<FString> EligibleDroneIDs = BuildEligibleOnlineCommandDroneIDs();
	if (EligibleDroneIDs.IsEmpty())
	{
		RefreshFleetSelectAll();
		return;
	}
	ReplaceCommandDroneSelection(
		IsExactCommandSelection(EligibleDroneIDs)
			? TArray<FString>{}
			: EligibleDroneIDs);
}

void UGCSUIBridgeSubsystem::HandleFleetSelectAllCheckStateChanged(
	const bool bIsChecked)
{
	if (bUpdatingFleetList)
	{
		return;
	}
	if (IsCommandSelectionMutationBlocked())
	{
		RefreshFleetSelectAll();
		return;
	}

	const TArray<FString> EligibleDroneIDs = BuildEligibleOnlineCommandDroneIDs();
	ReplaceCommandDroneSelection(
		bIsChecked && !EligibleDroneIDs.IsEmpty()
			? EligibleDroneIDs
			: TArray<FString>{});
}

void UGCSUIBridgeSubsystem::SetCommandDroneSelected(
	const FString& DroneID,
	const bool bSelected)
{
	if (DroneID.IsEmpty())
	{
		return;
	}
	if (IsCommandSelectionMutationBlocked())
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
	OnCommandSelectionChanged.Broadcast();
	RefreshFromFleet();

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
	if (IsCommandSelectionMutationBlocked())
	{
		return;
	}

	TSet<FString> Replacement;
	for (const FString& DroneID : DroneIDs)
	{
		if (!DroneID.IsEmpty())
		{
			Replacement.Add(DroneID);
		}
	}
	if (Replacement.Num() == SelectedCommandDroneIDs.Num())
	{
		bool bEqual = true;
		for (const FString& DroneID : Replacement)
		{
			if (!SelectedCommandDroneIDs.Contains(DroneID))
			{
				bEqual = false;
				break;
			}
		}
		if (bEqual)
		{
			return;
		}
	}

	SelectedCommandDroneIDs = MoveTemp(Replacement);
	SyncCommandSelectionToVisualization();
	OnCommandSelectionChanged.Broadcast();
	RefreshFromFleet();
}

TArray<FString> UGCSUIBridgeSubsystem::BuildEligibleOnlineCommandDroneIDs() const
{
	TArray<FString> Result;
	const UGameInstance* GameInstance = GetGameInstance();
	const UFleetManagerSubsystem* Fleet = GameInstance
		? GameInstance->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (!Fleet)
	{
		return Result;
	}

	const TArray<FDroneData> FleetSnapshot =
		Fleet->GetDronePage(0, Fleet->GetDroneCount());
	Result.Reserve(FleetSnapshot.Num());
	for (const FDroneData& Drone : FleetSnapshot)
	{
		if (Drone.bOnline && !Drone.DroneID.IsEmpty())
		{
			Result.Add(Drone.DroneID);
		}
	}
	Result.Sort();
	return Result;
}

bool UGCSUIBridgeSubsystem::IsExactCommandSelection(
	const TArray<FString>& EligibleDroneIDs) const
{
	if (EligibleDroneIDs.IsEmpty()
		|| EligibleDroneIDs.Num() != SelectedCommandDroneIDs.Num())
	{
		return false;
	}
	for (const FString& DroneID : EligibleDroneIDs)
	{
		if (!SelectedCommandDroneIDs.Contains(DroneID))
		{
			return false;
		}
	}
	return true;
}

bool UGCSUIBridgeSubsystem::IsCommandSelectionMutationBlocked() const
{
	return RouteWorkflowCoordinator
		&& RouteWorkflowCoordinator->IsCommandSelectionMutationBlocked();
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
	ResetTelemetryHorizontalDrag();
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
		ResetTelemetryHorizontalDrag();
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
		ResetTelemetryHorizontalDrag();
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

	UpdateTelemetryHorizontalScrollInput(PlayerController);
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

	if (bTelemetryHorizontalDragging
		|| IsPointerOverBlockingGCSUI(FSlateApplication::Get().GetCursorPos()))
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
	// Once a telemetry-table drag starts, it owns the right mouse button until
	// release even if the cursor leaves the scroll box. Camera look must not
	// begin halfway through that drag.
	if (bTelemetryHorizontalDragging)
	{
		return true;
	}

	// Slate interactables cover buttons, list rows, scroll bars and popup menu
	// entries, including combo-box menus that can extend outside their owner.
	if (IsPointerOverInteractableSlateWidget(ScreenPosition))
	{
		return true;
	}

	// These authored child widgets are compact panels, so their entire visible
	// rectangles are UI, including non-interactive padding/background space.
	for (const TWeakObjectPtr<UUserWidget>& WidgetRef :
		{ TopBarWidget,
		  LeftToolbarWidget,
		  FleetPanelWidget,
		  DroneDetailWidget,
		  RightToolbarWidget,
		  RightPanelWidget,
		  PlaybackWidget,
		  TelemetryFilterWidget })
	{
		const UUserWidget* Widget = WidgetRef.Get();
		if (IsVisibleWidgetUnderLocation(Widget, ScreenPosition))
		{
			return true;
		}
	}

	// Navigation pages are full-screen viewport overlays, while their authored
	// content occupies only part of the screen. Test the real panel instead of
	// the page root so transparent scene space remains available for marquee
	// selection and camera gestures. SelectionRectangle is intentionally absent:
	// it is scene feedback, not blocking UI.
	const auto IsUnderNavigationContent = [&ScreenPosition](
		const TWeakObjectPtr<UUserWidget>& PageRef,
		const FName ContentName)
	{
		const UUserWidget* Page = PageRef.Get();
		if (!Page || !Page->IsVisible() || !Page->WidgetTree)
		{
			return false;
		}
		return IsVisibleWidgetUnderLocation(
			Page->WidgetTree->FindWidget(ContentName),
			ScreenPosition);
	};
	if (IsUnderNavigationContent(MapPageWidget, TEXT("PANEL_Page"))
		|| IsUnderNavigationContent(LogsPageWidget, TEXT("VB_Page")))
	{
		return true;
	}

	const UUserWidget* ImportWidget = RouteImportWidget.Get();
	const UWidget* ImportBorder = ImportWidget && ImportWidget->IsVisible()
		&& ImportWidget->WidgetTree
		? ImportWidget->WidgetTree->FindWidget(TEXT("BORDER_RouteImport"))
		: nullptr;
	if (IsVisibleWidgetUnderLocation(ImportBorder, ScreenPosition))
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

void UGCSUIBridgeSubsystem::BindHoverTestControls()
{
	UnbindHoverTestControls();
	HoverTestContentWidget = FindRightPanelWidget(TEXT("VB_HoverTestContent"));
	HoverAltitudeInput = Cast<UEditableTextBox>(FindRightPanelWidget(TEXT("EDT_HoverAltitude")));
	HoverLayerCombo = Cast<UComboBoxString>(FindRightPanelWidget(TEXT("CMB_HoverLayer")));
	HoverGenerateLayersButton = FindRightPanelButton(TEXT("BTN_HoverGenerateLayers"));

#define BIND_HOVER_BUTTON(Name, Handler) \
	if (UButton* Button = FindRightPanelButton(Name)) \
	{ \
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::Handler); \
		Button->OnClicked.AddDynamic(this, &UGCSUIBridgeSubsystem::Handler); \
	}
	BIND_HOVER_BUTTON(TEXT("BTN_ConsoleHoverTest_1"), HandleHoverPanelToggleClicked);
	BIND_HOVER_BUTTON(TEXT("BTN_HoverGenerateLayers"), HandleHoverGenerateLayersClicked);
	BIND_HOVER_BUTTON(TEXT("BTN_HoverUnlock"), HandleHoverUnlockClicked);
	BIND_HOVER_BUTTON(TEXT("BTN_HoverTakeoff"), HandleHoverTakeoffClicked);
	BIND_HOVER_BUTTON(TEXT("BTN_HoverLand"), HandleHoverLandClicked);
	BIND_HOVER_BUTTON(TEXT("BTN_HoverLock"), HandleHoverLockClicked);
	BIND_HOVER_BUTTON(TEXT("BTN_HoverManual"), HandleHoverManualClicked);
#undef BIND_HOVER_BUTTON

	if (UEditableTextBox* Input = HoverAltitudeInput.Get())
	{
		Input->OnTextCommitted.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverAltitudeCommitted);
		Input->OnTextCommitted.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverAltitudeCommitted);
		Input->SetText(FText::AsNumber(HoverAltitudeMeters));
	}
	if (UComboBoxString* Combo = HoverLayerCombo.Get())
	{
		Combo->OnSelectionChanged.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverLayerSelectionChanged);
		Combo->OnSelectionChanged.AddDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverLayerSelectionChanged);
	}
	OnCommandSelectionChanged.RemoveAll(this);
	OnCommandSelectionChanged.AddUObject(this, &UGCSUIBridgeSubsystem::HandleHoverCommandSelectionChanged);
	ResetHoverLayerPlan();
}

void UGCSUIBridgeSubsystem::UnbindHoverTestControls()
{
#define UNBIND_HOVER_BUTTON(Name, Handler) \
	if (UButton* Button = FindRightPanelButton(Name)) \
	{ \
		Button->OnClicked.RemoveDynamic(this, &UGCSUIBridgeSubsystem::Handler); \
	}
	UNBIND_HOVER_BUTTON(TEXT("BTN_ConsoleHoverTest_1"), HandleHoverPanelToggleClicked);
	UNBIND_HOVER_BUTTON(TEXT("BTN_HoverGenerateLayers"), HandleHoverGenerateLayersClicked);
	UNBIND_HOVER_BUTTON(TEXT("BTN_HoverUnlock"), HandleHoverUnlockClicked);
	UNBIND_HOVER_BUTTON(TEXT("BTN_HoverTakeoff"), HandleHoverTakeoffClicked);
	UNBIND_HOVER_BUTTON(TEXT("BTN_HoverLand"), HandleHoverLandClicked);
	UNBIND_HOVER_BUTTON(TEXT("BTN_HoverLock"), HandleHoverLockClicked);
	UNBIND_HOVER_BUTTON(TEXT("BTN_HoverManual"), HandleHoverManualClicked);
#undef UNBIND_HOVER_BUTTON
	if (UEditableTextBox* Input = HoverAltitudeInput.Get())
	{
		Input->OnTextCommitted.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverAltitudeCommitted);
	}
	if (UComboBoxString* Combo = HoverLayerCombo.Get())
	{
		Combo->OnSelectionChanged.RemoveDynamic(this, &UGCSUIBridgeSubsystem::HandleHoverLayerSelectionChanged);
	}
	OnCommandSelectionChanged.RemoveAll(this);
}

void UGCSUIBridgeSubsystem::ResetHoverLayerPlan(const FString& Status)
{
	HoverLayers.Reset();
	HoverLayerBaseDroneIDs.Reset();
	HoverLayerOptions.Reset();
	HoverLayerFleetSessionID.Reset();
	HoverLayerPositionSource.Reset();
	ActiveHoverLayerOptionIndex = INDEX_NONE;
	HoverTestStatus = Status;
	if (UComboBoxString* Combo = HoverLayerCombo.Get())
	{
		bUpdatingHoverLayerCombo = true;
		Combo->ClearOptions();
		Combo->AddOption(TEXT("未生成"));
		Combo->SetSelectedOption(TEXT("未生成"));
		bUpdatingHoverLayerCombo = false;
	}
	RefreshHoverTestControls();
}

bool UGCSUIBridgeSubsystem::BuildHoverPositions(
	const TArray<FString>& DroneIDs,
	TArray<FHoverLayerPosition>& OutPositions,
	FString& OutError,
	FString* OutPositionSource) const
{
	OutPositions.Reset();
	OutError.Reset();
	if (OutPositionSource)
	{
		OutPositionSource->Reset();
	}
	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (!Fleet || DroneIDs.IsEmpty())
	{
		OutError = TEXT("没有可用于分层的在线无人机。");
		return false;
	}

	TArray<FDroneData> Drones;
	Drones.Reserve(DroneIDs.Num());
	bool bAllGpsValid = true;
	for (const FString& DroneID : DroneIDs)
	{
		FDroneData Drone;
		if (!Fleet->GetDroneByID(DroneID, Drone))
		{
			OutError = FString::Printf(TEXT("找不到无人机 %s。"), *DroneID);
			return false;
		}
		if (!Drone.bOnline)
		{
			OutError = FString::Printf(TEXT("无人机 %s 当前不在线。"), *DroneID);
			return false;
		}
		bAllGpsValid = bAllGpsValid && DroneGPS::IsValid(Drone);
		Drones.Add(MoveTemp(Drone));
	}

	if (bAllGpsValid)
	{
		const double OriginLatitudeRadians = FMath::DegreesToRadians(Drones[0].GPSLat);
		const double OriginLongitudeRadians = FMath::DegreesToRadians(Drones[0].GPSLon);
		const double SinLatitude = FMath::Sin(OriginLatitudeRadians);
		const double CosLatitude = FMath::Cos(OriginLatitudeRadians);
		const double SinLongitude = FMath::Sin(OriginLongitudeRadians);
		const double CosLongitude = FMath::Cos(OriginLongitudeRadians);
		const RouteAssignmentGeo::FEcefPosition OriginEcef =
			RouteAssignmentGeo::GeodeticToEcef(Drones[0].GPSLat, Drones[0].GPSLon, 0.0);

		OutPositions.Reserve(Drones.Num());
		for (const FDroneData& Drone : Drones)
		{
			const RouteAssignmentGeo::FEcefPosition Ecef =
				RouteAssignmentGeo::GeodeticToEcef(Drone.GPSLat, Drone.GPSLon, 0.0);
			const double DeltaX = Ecef.X - OriginEcef.X;
			const double DeltaY = Ecef.Y - OriginEcef.Y;
			const double DeltaZ = Ecef.Z - OriginEcef.Z;
			FHoverLayerPosition& Position = OutPositions.AddDefaulted_GetRef();
			Position.DroneID = Drone.DroneID;
			Position.EastMeters = -SinLongitude * DeltaX + CosLongitude * DeltaY;
			Position.NorthMeters = -SinLatitude * CosLongitude * DeltaX
				- SinLatitude * SinLongitude * DeltaY
				+ CosLatitude * DeltaZ;
		}
		if (OutPositionSource)
		{
			*OutPositionSource = TEXT("室外 GPS");
		}
		return true;
	}

	// Indoor mode: use the imported route's frame-0 Local ENU positions. A
	// frozen/working assignment takes precedence; canonical DroneNNN IDs retain
	// the legacy RouteSlot-number fallback when no assignment exists yet.
	const URoutePreviewSubsystem* RoutePreview = GetGameInstance()
		? GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>()
		: nullptr;
	const FRoutePreviewData* RouteData = RoutePreview && RoutePreview->IsRouteLoaded()
		? &RoutePreview->GetRouteData()
		: nullptr;
	TMap<FString, int32> RouteSlotByDroneID;
	if (const URouteAssignmentSubsystem* Assignment = GetGameInstance()
		? GetGameInstance()->GetSubsystem<URouteAssignmentSubsystem>()
		: nullptr)
	{
		FRouteAssignmentSnapshot Snapshot;
		if ((Assignment->GetFrozenSnapshot(Snapshot)
				|| Assignment->GetWorkingSnapshot(Snapshot))
			&& RoutePreview
			&& Snapshot.SourceIdentity.RouteDatasetGeneration
				== RoutePreview->GetRouteDatasetGeneration()
			&& Snapshot.SourceIdentity.FleetSessionID
				== Fleet->GetActiveFleetSessionID())
		{
			for (const FRouteAssignmentEntry& Entry : Snapshot.MatchedEntries)
			{
				RouteSlotByDroneID.Add(Entry.DroneID, Entry.RouteSlotIndex);
			}
		}
	}

	bool bHaveAllRoutePositions = RouteData != nullptr;
	if (RouteData)
	{
		OutPositions.Reserve(Drones.Num());
		for (const FDroneData& Drone : Drones)
		{
			const int32* AssignedSlot = RouteSlotByDroneID.Find(Drone.DroneID);
			const int32 RouteSlot = AssignedSlot
				? *AssignedSlot
				: ParseLogicalDroneNumber(Drone.DroneID);
			if (!RouteData->IsValidSample(0, RouteSlot))
			{
				bHaveAllRoutePositions = false;
				break;
			}
			const FVector3f& RoutePosition = RouteData->GetPosition(0, RouteSlot);
			if (!FMath::IsFinite(RoutePosition.X) || !FMath::IsFinite(RoutePosition.Y))
			{
				bHaveAllRoutePositions = false;
				break;
			}
			FHoverLayerPosition& Position = OutPositions.AddDefaulted_GetRef();
			Position.DroneID = Drone.DroneID;
			Position.EastMeters = RoutePosition.X;
			Position.NorthMeters = RoutePosition.Y;
		}
	}
	if (bHaveAllRoutePositions && OutPositions.Num() == Drones.Num())
	{
		if (OutPositionSource)
		{
			*OutPositionSource = TEXT("室内 NPZ 初始位置");
		}
		return true;
	}

	// Without any trustworthy horizontal position, isolate every drone by using
	// one coincident placeholder. The greedy model consequently emits one drone
	// per layer instead of rejecting indoor operation or guessing adjacency.
	OutPositions.Reset();
	OutPositions.Reserve(Drones.Num());
	for (const FDroneData& Drone : Drones)
	{
		FHoverLayerPosition& Position = OutPositions.AddDefaulted_GetRef();
		Position.DroneID = Drone.DroneID;
	}
	if (OutPositionSource)
	{
		*OutPositionSource = TEXT("室内单机独立层（无位置数据）");
	}
	return true;
}

bool UGCSUIBridgeSubsystem::TryReadHoverAltitude(FString& OutError)
{
	OutError.Reset();
	const UEditableTextBox* Input = HoverAltitudeInput.Get();
	if (!Input)
	{
		OutError = TEXT("未找到悬停高度输入框。 ");
		return false;
	}
	const FString ValueText = Input->GetText().ToString().TrimStartAndEnd();
	float ParsedValue = 0.0f;
	if (!LexTryParseString(ParsedValue, *ValueText) || !FMath::IsFinite(ParsedValue))
	{
		OutError = TEXT("请输入有效的悬停高度数值。");
		return false;
	}
	if (ParsedValue < 0.5f || ParsedValue > 120.0f)
	{
		OutError = TEXT("悬停高度范围为 0.5～120.0 m。");
		return false;
	}
	HoverAltitudeMeters = ParsedValue;
	if (UEditableTextBox* MutableInput = HoverAltitudeInput.Get())
	{
		MutableInput->SetText(FText::FromString(FString::Printf(TEXT("%.1f"), HoverAltitudeMeters)));
	}
	RefreshHoverMVPControls();
	return true;
}

void UGCSUIBridgeSubsystem::ApplyHoverLayerSelection(const int32 ComboOptionIndex)
{
	if (!HoverLayerOptions.IsValidIndex(ComboOptionIndex))
	{
		return;
	}
	if (IsCommandSelectionMutationBlocked())
	{
		HoverTestStatus = TEXT("当前任务已锁定选择，暂时不能切换分层。");
		RefreshHoverTestControls();
		return;
	}

	const TArray<FString>* TargetIDs = ComboOptionIndex == 0
		? &HoverLayerBaseDroneIDs
		: (HoverLayers.IsValidIndex(ComboOptionIndex - 1)
			? &HoverLayers[ComboOptionIndex - 1]
			: nullptr);
	if (!TargetIDs)
	{
		return;
	}
	ActiveHoverLayerOptionIndex = ComboOptionIndex;
	bApplyingHoverLayerSelection = true;
	ReplaceCommandDroneSelection(*TargetIDs);
	bApplyingHoverLayerSelection = false;

	if (UComboBoxString* Combo = HoverLayerCombo.Get())
	{
		bUpdatingHoverLayerCombo = true;
		Combo->SetSelectedOption(HoverLayerOptions[ComboOptionIndex]);
		bUpdatingHoverLayerCombo = false;
	}
	HoverTestStatus = ComboOptionIndex == 0
		? TEXT("已恢复全部分层目标；请选择具体层后再执行悬停动作。")
		: FString::Printf(TEXT("当前已选择第 %d 层。"), ComboOptionIndex);
	RefreshHoverTestControls();
}

void UGCSUIBridgeSubsystem::HandleHoverCommandSelectionChanged()
{
	if (bApplyingHoverLayerSelection || HoverLayers.IsEmpty())
	{
		return;
	}
	const TArray<FString> SelectedIDs = GetCommandTargetDroneIDs();
	int32 MatchingOption = HaveSameDroneIDs(SelectedIDs, HoverLayerBaseDroneIDs) ? 0 : INDEX_NONE;
	for (int32 Index = 0; MatchingOption == INDEX_NONE && Index < HoverLayers.Num(); ++Index)
	{
		if (HaveSameDroneIDs(SelectedIDs, HoverLayers[Index]))
		{
			MatchingOption = Index + 1;
		}
	}
	ActiveHoverLayerOptionIndex = MatchingOption;
	if (MatchingOption == INDEX_NONE)
	{
		HoverTestStatus = TEXT("当前选择已改变；请重新选择一个分层或重新生成分层。");
	}
	else if (UComboBoxString* Combo = HoverLayerCombo.Get())
	{
		bUpdatingHoverLayerCombo = true;
		Combo->SetSelectedOption(HoverLayerOptions[MatchingOption]);
		bUpdatingHoverLayerCombo = false;
	}
	RefreshHoverTestControls();
}

bool UGCSUIBridgeSubsystem::PrepareHoverLayerAction(
	FString& OutError,
	const bool bRequireCurrentSpacing)
{
	OutError.Reset();
	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (!Fleet || HoverLayers.IsEmpty())
	{
		OutError = TEXT("请先生成分层。");
		return false;
	}
	if (Fleet->GetActiveFleetSessionID() != HoverLayerFleetSessionID)
	{
		OutError = TEXT("机群会话已变化，请重新生成分层。");
		return false;
	}
	if (ActiveHoverLayerOptionIndex <= 0
		|| !HoverLayers.IsValidIndex(ActiveHoverLayerOptionIndex - 1))
	{
		OutError = TEXT("请选择一个具体分层后再执行操作。");
		return false;
	}
	const TArray<FString>& ActiveLayer = HoverLayers[ActiveHoverLayerOptionIndex - 1];
	if (!HaveSameDroneIDs(GetCommandTargetDroneIDs(), ActiveLayer))
	{
		OutError = TEXT("当前选择与分层不一致，请重新选择分层。");
		return false;
	}
	if (!bRequireCurrentSpacing)
	{
		return true;
	}
	TArray<FHoverLayerPosition> CurrentPositions;
	if (!BuildHoverPositions(ActiveLayer, CurrentPositions, OutError))
	{
		return false;
	}
	return HoverLayerSelection::ValidateLayerSpacing(
		CurrentPositions,
		HoverLayerMinimumSpacingMeters,
		OutError);
}

void UGCSUIBridgeSubsystem::RefreshHoverTestControls()
{
	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (Fleet && !HoverLayerFleetSessionID.IsEmpty()
		&& Fleet->GetActiveFleetSessionID() != HoverLayerFleetSessionID)
	{
		ResetHoverLayerPlan(TEXT("机群会话已变化，请重新生成分层。"));
		return;
	}

	SetRightPanelText(
		TEXT("TXT_HoverTargetSummary"),
		FString::Printf(TEXT("目标：当前选择 %d 架"), GetCommandTargetDroneIDs().Num()));
	SetRightPanelText(
		TEXT("TXT_HoverLayerSummary"),
		HoverLayers.IsEmpty()
			? FString::Printf(TEXT("尚未生成分层｜安全间距 %.1f m"), HoverLayerMinimumSpacingMeters)
			: FString::Printf(
				TEXT("已生成 %d 层｜共 %d 架｜安全间距 %.1f m｜%s"),
				HoverLayers.Num(),
				HoverLayerBaseDroneIDs.Num(),
				HoverLayerMinimumSpacingMeters,
				*HoverLayerPositionSource));
	SetRightPanelText(TEXT("TXT_HoverTestStatus"), HoverTestStatus);

	const UGameInstance* GameInstance = GetGameInstance();
	const UDSPGCSProductionCommandProviderSubsystem* Commands = GameInstance
		? GameInstance->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>()
		: nullptr;
	const UDSPGCSProductionTelemetryProviderSubsystem* Telemetry = GameInstance
		? GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>()
		: nullptr;
	const bool bProductionReady = Commands && Commands->IsEnabled()
		&& Telemetry && Telemetry->IsEnabled()
		&& Telemetry->IsTelemetryLinkConnected();
	const bool bLayerActionReady = bProductionReady
		&& ActiveHoverLayerOptionIndex > 0
		&& HoverLayers.IsValidIndex(ActiveHoverLayerOptionIndex - 1)
		&& HaveSameDroneIDs(
			GetCommandTargetDroneIDs(),
			HoverLayers[ActiveHoverLayerOptionIndex - 1]);
	for (const FName ButtonName :
		{ FName(TEXT("BTN_HoverUnlock")),
		  FName(TEXT("BTN_HoverTakeoff")),
		  FName(TEXT("BTN_HoverLand")),
		  FName(TEXT("BTN_HoverLock")),
		  FName(TEXT("BTN_HoverManual")) })
	{
		if (UButton* Button = FindRightPanelButton(ButtonName))
		{
			Button->SetIsEnabled(bLayerActionReady);
		}
	}
	if (UButton* Button = HoverGenerateLayersButton.Get())
	{
		Button->SetIsEnabled(!IsCommandSelectionMutationBlocked());
	}
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
	// The production provider has already published the matching MDHK cache
	// before this batch notification. Refresh the table immediately; the 1 Hz
	// UI tick remains only as a fallback for view creation/timing races.
	RefreshTelemetryTableFromProduction();
	TelemetryTableRefreshAccumulator = 0.0f;
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
		  FName(TEXT("BTN_ModeAutonomous")),
		  FName(TEXT("BTN_TimeStart")),
		  FName(TEXT("BTN_TimeStop")),
		  FName(TEXT("BTN_ExecuteDanceStep")),
		  FName(TEXT("BTN_EndDanceStep")) })
	{
		UButton* Button = FindButton(ButtonName);
		if (!Button)
		{
			Button = FindRightPanelButton(ButtonName);
		}
		if (Button)
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

	if (FleetListView.IsValid())
	{
		FleetPageOffset = 0;
		RefreshFleetListRows(
			Fleet->GetDronePage(0, TotalCount),
			TotalCount,
			Fleet->GetCurrentSelectedDroneID());
	}
	else
	{
		const TArray<FDroneData> VisibleDrones =
			Fleet->GetDronePage(FleetPageOffset, FleetPageSize);
		RefreshFleetRows(
			VisibleDrones,
			TotalCount,
			Fleet->GetCurrentSelectedDroneID());
	}
	RefreshFleetSelectAll();

	FDroneData SelectedDrone;
	const bool bHasDetailSelection = Fleet->GetSelectedDrone(SelectedDrone);
	// Fleet data chooses which drone's values are presented, but it never owns
	// the control panel's visibility. Only the toolbar and menu handlers do.
	if (bHasDetailSelection)
	{
		UpdateDroneInfo(SelectedDrone);
	}
	RefreshHoverTestControls();
}

void UGCSUIBridgeSubsystem::RefreshFleetListRows(
	const TArray<FDroneData>& Drones,
	const int32 TotalCount,
	const FString& CurrentSelectedDroneID)
{
	UListView* List = FleetListView.Get();
	if (!List)
	{
		return;
	}

	SetFleetText(
		TEXT("FleetCount"),
		FString::FromInt(FMath::Max(0, TotalCount)));
	SetFleetText(
		TEXT("TXT_FleetSelectedCount"),
		FString::Printf(TEXT("选中 %d"), SelectedCommandDroneIDs.Num()));

	UClass* RowDataClass = LoadObject<UClass>(
		nullptr,
		TEXT("/Game/UI/BP_FleetRowData.BP_FleetRowData_C"));
	if (!RowDataClass)
	{
		UE_LOG(LogTemp, Error, TEXT("BP_FleetRowData class could not be loaded."));
		return;
	}

	TMap<FString, int32> SatellitesByDroneID;
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (const UDSPGCSProductionTelemetryProviderSubsystem* Telemetry =
			GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>())
		{
			for (const FMDHKTelemetryTableRow& Row : Telemetry->GetMDHKTelemetryRows())
			{
				if (Row.SatellitesVisible >= 0)
				{
					SatellitesByDroneID.Add(Row.DroneID, Row.SatellitesVisible);
				}
			}
		}
	}

	TArray<UObject*> OrderedItems;
	OrderedItems.Reserve(Drones.Num());
	TSet<FString> CurrentDroneIDs;
	CurrentDroneIDs.Reserve(Drones.Num());
	bool bPropertiesValid = true;
	TSet<UObject*> ChangedRowObjects;
	const bool bSelectionLocked = IsCommandSelectionMutationBlocked();
	const EDSPGCSTelemetryLinkState ServerState = GetProductionTelemetryLinkState();

	for (const FDroneData& Drone : Drones)
	{
		if (Drone.DroneID.IsEmpty())
		{
			continue;
		}
		CurrentDroneIDs.Add(Drone.DroneID);

		TObjectPtr<UObject>& StoredObject = FleetListRowObjects.FindOrAdd(Drone.DroneID);
		if (!StoredObject)
		{
			StoredObject = NewObject<UObject>(this, RowDataClass);
		}
		UObject* RowObject = StoredObject.Get();
		OrderedItems.Add(RowObject);
		bool bRowChanged = false;

		const int32* Satellites = SatellitesByDroneID.Find(Drone.DroneID);
		const FString GPSText = Satellites
			? FString::FromInt(*Satellites)
			: (DroneGPS::IsValid(Drone) ? TEXT("OK") : TEXT("--"));
		const EGCSDroneDisplayStatus DisplayStatus =
			ResolveDroneDisplayStatus(ServerState, Drone.bOnline);
		bPropertiesValid =
			SetFleetString(RowObject, TEXT("DroneID"), Drone.DroneID, bRowChanged) &&
			SetFleetString(RowObject, TEXT("Mode"), Drone.Mode, bRowChanged) &&
			SetFleetString(RowObject, TEXT("GPSText"), GPSText, bRowChanged) &&
			SetFleetString(RowObject, TEXT("BatteryText"),
				FString::Printf(TEXT("%.0f%%"), Drone.Battery), bRowChanged) &&
			SetFleetString(RowObject, TEXT("HeightText"),
				FString::Printf(TEXT("%.1f"), Drone.Altitude), bRowChanged) &&
			SetFleetString(RowObject, TEXT("RSSIText"),
				FString::FromInt(Drone.RSSI), bRowChanged) &&
			SetFleetString(RowObject, TEXT("StatusText"),
				FGCSUIWidgetPresenter::DisplayStatusText(DisplayStatus), bRowChanged) &&
			SetFleetBool(RowObject, TEXT("bCommandSelected"),
				SelectedCommandDroneIDs.Contains(Drone.DroneID), bRowChanged) &&
			SetFleetBool(RowObject, TEXT("bCommandEnabled"),
				Drone.bOnline && !bSelectionLocked, bRowChanged) &&
			bPropertiesValid;
		if (bRowChanged)
		{
			ChangedRowObjects.Add(RowObject);
		}
	}

	for (auto It = FleetListRowObjects.CreateIterator(); It; ++It)
	{
		if (!CurrentDroneIDs.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}

	bool bItemsChanged = List->GetNumItems() != OrderedItems.Num();
	if (!bItemsChanged)
	{
		const TArray<UObject*>& ExistingItems = List->GetListItems();
		for (int32 Index = 0; Index < OrderedItems.Num(); ++Index)
		{
			if (ExistingItems[Index] != OrderedItems[Index])
			{
				bItemsChanged = true;
				break;
			}
		}
	}

	if (!bPropertiesValid)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("BP_FleetRowData variables no longer match the Fleet ListView contract."));
		return;
	}

	TGuardValue<bool> UpdatingGuard(bUpdatingFleetList, true);
	if (bItemsChanged)
	{
		List->SetListItems(OrderedItems);
	}

	const bool bDetailSelectionChanged =
		!LastRenderedFleetDetailDroneID.Equals(
			CurrentSelectedDroneID,
			ESearchCase::IgnoreCase);
	LastRenderedFleetDetailDroneID = CurrentSelectedDroneID;

	// Detail selection is intentionally not mirrored into UListView selection.
	// The row's BTN_FleetRowDetail owns detail clicks, while the checkbox owns
	// command selection. Telemetry may update row data but can never overwrite a
	// click with the previously selected ListView item.
	if (bItemsChanged || bDetailSelectionChanged || !ChangedRowObjects.IsEmpty())
	{
		for (UGCSFleetRowCheckBinding* Binding : FleetListCheckBindings)
		{
			if (Binding
				&& (bItemsChanged
					|| bDetailSelectionChanged
					|| ChangedRowObjects.Contains(Binding->GetRowObject())))
			{
				Binding->RefreshFromRowObject(CurrentSelectedDroneID);
			}
		}
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

void UGCSUIBridgeSubsystem::RefreshFleetSelectAll()
{
	const TArray<FString> EligibleDroneIDs = BuildEligibleOnlineCommandDroneIDs();
	if (UCheckBox* CheckBox = FleetSelectAllCheckBox.Get())
	{
		TGuardValue<bool> UpdatingGuard(bUpdatingFleetList, true);
		CheckBox->SetIsChecked(IsExactCommandSelection(EligibleDroneIDs));
		CheckBox->SetIsEnabled(
			!EligibleDroneIDs.IsEmpty() && !IsCommandSelectionMutationBlocked());
		return;
	}

	SetFleetText(
		TEXT("BTN_FleetSelectALL_Text"),
		IsExactCommandSelection(EligibleDroneIDs) ? TEXT("✓") : TEXT("X"));
	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetSelectALL")))
	{
		Button->SetIsEnabled(
			!EligibleDroneIDs.IsEmpty() && !IsCommandSelectionMutationBlocked());
	}
}

UButton* UGCSUIBridgeSubsystem::FindRightPanelButton(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindButton(RightPanelWidget.Get(), Name);
}

USpinBox* UGCSUIBridgeSubsystem::FindRightPanelSpinBox(const FName Name) const
{
	UUserWidget* Panel = RightPanelWidget.Get();
	return Panel && Panel->WidgetTree
		? Cast<USpinBox>(Panel->WidgetTree->FindWidget(Name))
		: nullptr;
}

UCheckBox* UGCSUIBridgeSubsystem::FindRightPanelCheckBox(const FName Name) const
{
	UUserWidget* Panel = RightPanelWidget.Get();
	return Panel && Panel->WidgetTree
		? Cast<UCheckBox>(Panel->WidgetTree->FindWidget(Name))
		: nullptr;
}

UTextBlock* UGCSUIBridgeSubsystem::FindRightPanelText(const FName Name) const
{
	return FGCSUIWidgetPresenter::FindText(RightPanelWidget.Get(), Name);
}

UWidget* UGCSUIBridgeSubsystem::FindRightPanelWidget(const FName Name) const
{
	UUserWidget* Panel = RightPanelWidget.Get();
	return Panel && Panel->WidgetTree ? Panel->WidgetTree->FindWidget(Name) : nullptr;
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

void UGCSUIBridgeSubsystem::SetRightPanelText(
	const FName Name,
	const FString& Value) const
{
	FGCSUIWidgetPresenter::SetText(RightPanelWidget.Get(), Name, Value);
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

void UGCSUIBridgeSubsystem::BindTimeControlControls()
{
	UButton* StartButton = FindRightPanelButton(TEXT("BTN_TimeStart"));
	UButton* StopButton = FindRightPanelButton(TEXT("BTN_TimeStop"));
	UButton* ExecuteButton = FindRightPanelButton(TEXT("BTN_ExecuteDanceStep"));
	UButton* EndButton = FindRightPanelButton(TEXT("BTN_EndDanceStep"));
	USpinBox* HourInput = FindRightPanelSpinBox(TEXT("SPN_TimeHour"));
	USpinBox* MinuteInput = FindRightPanelSpinBox(TEXT("SPN_TimeMinute"));
	USpinBox* SecondInput = FindRightPanelSpinBox(TEXT("SPN_TimeSecond"));
	UTextBlock* DurationLabel = FindRightPanelText(TEXT("TXT_TimeCountdownLabel"));
	UTextBlock* CountdownText = FindRightPanelText(TEXT("TXT_TimeCountdown"));
	UTextBlock* StatusText = FindRightPanelText(TEXT("TXT_TimeControlStatus"));

	if (!StartButton || !StopButton || !ExecuteButton || !EndButton ||
		!HourInput || !MinuteInput || !SecondInput
		|| !DurationLabel || !CountdownText || !StatusText)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("[TimeControlUI] Asset mismatch Start=%s Stop=%s Execute=%s End=%s Hour=%s Minute=%s Second=%s DurationLabel=%s Duration=%s Status=%s"),
			StartButton ? TEXT("YES") : TEXT("NO"),
			StopButton ? TEXT("YES") : TEXT("NO"),
			ExecuteButton ? TEXT("YES") : TEXT("NO"),
			EndButton ? TEXT("YES") : TEXT("NO"),
			HourInput ? TEXT("YES") : TEXT("NO"),
			MinuteInput ? TEXT("YES") : TEXT("NO"),
			SecondInput ? TEXT("YES") : TEXT("NO"),
			DurationLabel ? TEXT("YES") : TEXT("NO"),
			CountdownText ? TEXT("YES") : TEXT("NO"),
			StatusText ? TEXT("YES") : TEXT("NO"));
		return;
	}

	StartButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleTimeStartClicked);
	StartButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleTimeStartClicked);
	StopButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleTimeStopClicked);
	StopButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleTimeStopClicked);
	ExecuteButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleExecuteDanceStepClicked);
	ExecuteButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleExecuteDanceStepClicked);
	EndButton->OnClicked.RemoveDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleEndDanceStepClicked);
	EndButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleEndDanceStepClicked);

	const auto ConfigureTimeInput = [](USpinBox* Input, const float Maximum)
	{
		Input->SetMinValue(0.0f);
		Input->SetMaxValue(Maximum);
		Input->SetMinSliderValue(0.0f);
		Input->SetMaxSliderValue(Maximum);
		Input->SetDelta(1.0f);
	};
	ConfigureTimeInput(HourInput, 23.0f);
	ConfigureTimeInput(MinuteInput, 59.0f);
	ConfigureTimeInput(SecondInput, 59.0f);

	const FDateTime InitialTime = FDateTime::Now() + FTimespan::FromSeconds(60.0);
	HourInput->SetValue(static_cast<float>(InitialTime.GetHour()));
	MinuteInput->SetValue(static_cast<float>(InitialTime.GetMinute()));
	SecondInput->SetValue(static_cast<float>(InitialTime.GetSecond()));
	DurationLabel->SetText(FText::FromString(TEXT("倒计时")));
	CountdownText->SetText(FText::FromString(TEXT("--:--:--")));
	CountdownText->SetToolTipText(FText::FromString(
		TEXT("授时任务等待期间显示剩余倒计时；到点后切换为表演时间并按NPZ计划正计时。")));
	StatusText->SetText(FText::FromString(TEXT("未设置授时任务")));

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[TimeControlUI] Bound message=78 LEN=5 CRC=145 DefaultLocalTime=%02d:%02d:%02d"),
		InitialTime.GetHour(),
		InitialTime.GetMinute(),
		InitialTime.GetSecond());
}

void UGCSUIBridgeSubsystem::BindReferenceAndMapControls()
{
	UnbindReferenceAndMapControls();

	UButton* ApplyPileButton = FindRightPanelButton(TEXT("BTN_ApplyFixedPile"));
	UButton* BenchmarkExportButton = FindRightPanelButton(TEXT("BTN_DaoChu"));
	USpinBox* PileInput = FindRightPanelSpinBox(TEXT("SPN_FixedPileDroneID"));
	UTextBlock* PileStatus = FindRightPanelText(TEXT("TXT_FixedPileStatus"));
	UTextBlock* BenchmarkExportStatus = FindRightPanelText(TEXT("TXT_BenchmarkExportStatus"));
	UCheckBox* MapCheckBox = FindRightPanelCheckBox(TEXT("CheckBox_Map"));
	if (!ApplyPileButton
		|| !BenchmarkExportButton
		|| !PileInput
		|| !PileStatus
		|| !BenchmarkExportStatus
		|| !MapCheckBox)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("[GCSReferenceUI] Asset mismatch Apply=%s Export=%s Input=%s PileStatus=%s ExportStatus=%s Map=%s"),
			ApplyPileButton ? TEXT("YES") : TEXT("NO"),
			BenchmarkExportButton ? TEXT("YES") : TEXT("NO"),
			PileInput ? TEXT("YES") : TEXT("NO"),
			PileStatus ? TEXT("YES") : TEXT("NO"),
			BenchmarkExportStatus ? TEXT("YES") : TEXT("NO"),
			MapCheckBox ? TEXT("YES") : TEXT("NO"));
		return;
	}

	ApplyPileButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleFixedPileApplyClicked);
	BenchmarkExportButton->OnClicked.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleBenchmarkExportClicked);
	MapCheckBox->OnCheckStateChanged.AddDynamic(
		this,
		&UGCSUIBridgeSubsystem::HandleMapCheckStateChanged);
	PileInput->SetMinValue(0.0f);
	PileInput->SetMaxValue(99999.0f);
	PileInput->SetMinSliderValue(0.0f);
	PileInput->SetMaxSliderValue(99999.0f);
	PileInput->SetDelta(1.0f);
	SyncMapCheckBoxFromScene();
}

void UGCSUIBridgeSubsystem::UnbindReferenceAndMapControls()
{
	if (UButton* ApplyPileButton = FindRightPanelButton(TEXT("BTN_ApplyFixedPile")))
	{
		ApplyPileButton->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleFixedPileApplyClicked);
	}
	if (UButton* BenchmarkExportButton = FindRightPanelButton(TEXT("BTN_DaoChu")))
	{
		BenchmarkExportButton->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleBenchmarkExportClicked);
	}
	if (UCheckBox* MapCheckBox = FindRightPanelCheckBox(TEXT("CheckBox_Map")))
	{
		MapCheckBox->OnCheckStateChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleMapCheckStateChanged);
	}
}

ADroneFleetVisualizationActor* UGCSUIBridgeSubsystem::FindFleetVisualizationActor() const
{
	UWorld* World = MainWidgetRef.IsValid() ? MainWidgetRef->GetWorld() : GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

bool UGCSUIBridgeSubsystem::InitializeCoordinateAxes(
	const FVector& OriginWorld,
	const FVector& EastWorld,
	const FVector& NorthWorld,
	const int32 OriginDroneNumber,
	const float AxisLengthMeters) const
{
	UWorld* World = MainWidgetRef.IsValid() ? MainWidgetRef->GetWorld() : GetWorld();
	if (!World)
	{
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		UFunction* Function = Actor
			? Actor->FindFunction(TEXT("InitializeCoordinateAxes"))
			: nullptr;
		if (!Function)
		{
			continue;
		}

		FStructOnScope Parameters(Function);
		void* ParameterMemory = Parameters.GetStructMemory();
		FNumericProperty* OriginIDProperty =
			FindFProperty<FNumericProperty>(Function, TEXT("OriginDroneID"));
		FNumericProperty* AxisLengthProperty =
			FindFProperty<FNumericProperty>(Function, TEXT("AxisLengthMeters"));
		const bool bSignatureMatches = ParameterMemory
			&& SetVectorFunctionParameter(
				Function,
				ParameterMemory,
				TEXT("OriginWorldLocation"),
				OriginWorld)
			&& SetVectorFunctionParameter(
				Function,
				ParameterMemory,
				TEXT("EastWorldDirection"),
				EastWorld)
			&& SetVectorFunctionParameter(
				Function,
				ParameterMemory,
				TEXT("NorthWorldDirection"),
				NorthWorld)
			&& OriginIDProperty
			&& OriginIDProperty->IsInteger()
			&& AxisLengthProperty
			&& AxisLengthProperty->IsFloatingPoint();
		if (!bSignatureMatches)
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("[GCSFixedPile] InitializeCoordinateAxes signature mismatch on %s"),
				*Actor->GetName());
			return false;
		}

		OriginIDProperty->SetIntPropertyValue(
			OriginIDProperty->ContainerPtrToValuePtr<void>(ParameterMemory),
			static_cast<int64>(OriginDroneNumber));
		AxisLengthProperty->SetFloatingPointPropertyValue(
			AxisLengthProperty->ContainerPtrToValuePtr<void>(ParameterMemory),
			AxisLengthMeters);
		Actor->SetActorHiddenInGame(false);
		Actor->ProcessEvent(Function, ParameterMemory);
		TInlineComponentArray<UPrimitiveComponent*> AxisPrimitives(Actor);
		for (UPrimitiveComponent* Primitive : AxisPrimitives)
		{
			if (Primitive)
			{
				Primitive->SetDepthPriorityGroup(SDPG_World);
				Primitive->SetTranslucentSortPriority(-10);
				if (Primitive->GetFName() == TEXT("SM_Origin"))
				{
					// The selected drone sphere already represents the origin.
					Primitive->SetHiddenInGame(true);
				}
			}
		}
		return true;
	}
	return false;
}

TArray<AActor*> UGCSUIBridgeSubsystem::FindSceneMapActors() const
{
	TArray<AActor*> Result;
	UWorld* World = MainWidgetRef.IsValid() ? MainWidgetRef->GetWorld() : GetWorld();
	if (!World)
	{
		return Result;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (IsSceneMapActor(*It))
		{
			Result.Add(*It);
		}
	}
	return Result;
}

void UGCSUIBridgeSubsystem::SetSceneMapVisible(const bool bVisible) const
{
	const TArray<AActor*> MapActors = FindSceneMapActors();
	for (AActor* Actor : MapActors)
	{
		if (IsValid(Actor))
		{
			Actor->SetActorHiddenInGame(!bVisible);
		}
	}
	if (MapActors.IsEmpty())
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[GCSSceneMap] Visible=%s but no matching map actor was found"),
			bVisible ? TEXT("true") : TEXT("false"));
	}
	else
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[GCSSceneMap] Visible=%s Actors=%d"),
			bVisible ? TEXT("true") : TEXT("false"),
			MapActors.Num());
	}
}

void UGCSUIBridgeSubsystem::SyncMapCheckBoxFromScene()
{
	UCheckBox* MapCheckBox = FindRightPanelCheckBox(TEXT("CheckBox_Map"));
	if (!MapCheckBox)
	{
		return;
	}
	const TArray<AActor*> MapActors = FindSceneMapActors();
	bool bAnyMapVisible = false;
	for (const AActor* Actor : MapActors)
	{
		bAnyMapVisible |= IsValid(Actor) && !Actor->IsHidden();
	}
	bUpdatingMapCheckBoxFromScene = true;
	MapCheckBox->SetIsChecked(bAnyMapVisible);
	bUpdatingMapCheckBoxFromScene = false;
}

void UGCSUIBridgeSubsystem::HandleFixedPileApplyClicked()
{
	USpinBox* PileInput = FindRightPanelSpinBox(TEXT("SPN_FixedPileDroneID"));
	UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	ADroneFleetVisualizationActor* Visualization = FindFleetVisualizationActor();
	if (!PileInput || !Fleet || !Visualization)
	{
		SetRightPanelText(TEXT("TXT_FixedPileStatus"), TEXT("桩基应用失败：运行组件不可用"));
		return;
	}
	if (Visualization->GetVisualizationMode() != EGCSVisualizationMode::Live)
	{
		SetRightPanelText(TEXT("TXT_FixedPileStatus"), TEXT("请先退出航线预览，再设置桩基"));
		return;
	}

	const int32 DroneNumber = FMath::RoundToInt(PileInput->GetValue());
	if (DroneNumber < 0)
	{
		SetRightPanelText(TEXT("TXT_FixedPileStatus"), TEXT("桩基编号不能为负数"));
		return;
	}
	Visualization->RefreshFromFleet();
	FString DroneID;
	if (!Visualization->TryResolveDroneIDFromDisplayID(DroneNumber, DroneID))
	{
		SetRightPanelText(
			TEXT("TXT_FixedPileStatus"),
			FString::Printf(
				TEXT("桩基应用失败：当前显示编号 %d 不存在或不唯一"),
				DroneNumber));
		return;
	}

	FString Error;
	if (!Fleet->SetGeoOriginFromDroneID(DroneID, Error))
	{
		SetRightPanelText(
			TEXT("TXT_FixedPileStatus"),
			FString::Printf(TEXT("桩基应用失败：%s"), *Error));
		return;
	}

	Visualization->SetFixedPileDroneID(DroneID);
	Visualization->RefreshFromFleet();
	FVector OriginWorld = FVector::ZeroVector;
	FVector EastWorld = FVector::ZeroVector;
	FVector NorthWorld = FVector::ZeroVector;
	FVector UpWorld = FVector::ZeroVector;
	if (!Visualization->TryGetGeographicTangentBasis(
		OriginWorld,
		EastWorld,
		NorthWorld,
		UpWorld))
	{
		SetRightPanelText(
			TEXT("TXT_FixedPileStatus"),
			FString::Printf(TEXT("当前桩基：%s；坐标方向初始化失败"), *DroneID));
		return;
	}
	if (!InitializeCoordinateAxes(
		OriginWorld - UpWorld * FixedPileAxisDepthOffsetUU,
		EastWorld,
		NorthWorld,
		0,
		FixedPileAxisLengthMeters))
	{
		SetRightPanelText(
			TEXT("TXT_FixedPileStatus"),
			FString::Printf(TEXT("当前桩基：%s；未找到兼容的坐标轴蓝图"), *DroneID));
		return;
	}

	SetRightPanelText(
		TEXT("TXT_FixedPileStatus"),
		FString::Printf(TEXT("当前桩基：%s（局部原点）"), *DroneID));
}

void UGCSUIBridgeSubsystem::HandleBenchmarkExportClicked()
{
	constexpr int32 LegacyOriginDisplayID = 0;
	const FName StatusName(TEXT("TXT_BenchmarkExportStatus"));
	UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	ADroneFleetVisualizationActor* Visualization = FindFleetVisualizationActor();
	if (!Fleet || !Visualization)
	{
		SetRightPanelText(StatusName, TEXT("导出失败：运行组件不可用"));
		return;
	}
	if (Visualization->GetVisualizationMode() != EGCSVisualizationMode::Live)
	{
		SetRightPanelText(StatusName, TEXT("请先退出航线预览，再导出基准文件"));
		return;
	}
	if (Visualization->HasConfirmedRouteSlotDisplayOverrides())
	{
		SetRightPanelText(StatusName, TEXT("请先卸载已确认的NPZ编号，再导出基准文件"));
		return;
	}

	const FFleetGeoOrigin GeoOrigin = Fleet->GetFleetGeoOrigin();
	if (!GeoOrigin.bOriginInitialized || GeoOrigin.OriginDroneID.IsEmpty())
	{
		SetRightPanelText(StatusName, TEXT("请先应用桩基，再导出基准文件"));
		return;
	}

	const int64 SnapshotRevision = Fleet->GetFleetRevision();
	Visualization->RefreshFromFleet();
	TMap<FString, int32> DisplayIDsByDroneID;
	FString Error;
	if (!Visualization->TryGetLiveDisplayIDSnapshot(DisplayIDsByDroneID, Error))
	{
		SetRightPanelText(StatusName, FString::Printf(TEXT("导出失败：%s"), *Error));
		return;
	}
	const int32* OriginDisplayID = DisplayIDsByDroneID.Find(GeoOrigin.OriginDroneID);
	if (!OriginDisplayID || *OriginDisplayID != LegacyOriginDisplayID)
	{
		SetRightPanelText(StatusName, TEXT("导出失败：当前桩基不是编号0，请重新应用桩基"));
		return;
	}

	FVector OriginWorld = FVector::ZeroVector;
	FVector EastWorld = FVector::ZeroVector;
	FVector NorthWorld = FVector::ZeroVector;
	FVector UpWorld = FVector::ZeroVector;
	if (!Visualization->TryGetGeographicTangentBasis(
		OriginWorld,
		EastWorld,
		NorthWorld,
		UpWorld))
	{
		SetRightPanelText(StatusName, TEXT("导出失败：局部坐标方向尚未初始化"));
		return;
	}

	const TArray<FString> DroneIDs = Visualization->GetParticleDroneIDs();
	const TArray<FVector> DronePositions = Visualization->GetDronePositions();
	if (DroneIDs.IsEmpty() || DroneIDs.Num() != DronePositions.Num())
	{
		SetRightPanelText(StatusName, TEXT("导出失败：无人机位置快照不完整"));
		return;
	}

	TArray<FGCSBenchmarkExportRow> Rows;
	Rows.Reserve(DroneIDs.Num());
	for (int32 Index = 0; Index < DroneIDs.Num(); ++Index)
	{
		const FString& DroneID = DroneIDs[Index];
		FDroneData Drone;
		if (!Fleet->GetDroneByID(DroneID, Drone))
		{
			SetRightPanelText(
				StatusName,
				FString::Printf(TEXT("导出失败：无人机 %s 数据不存在"), *DroneID));
			return;
		}
		if (!Drone.bOnline)
		{
			SetRightPanelText(
				StatusName,
				FString::Printf(TEXT("导出失败：无人机 %s 当前离线"), *DroneID));
			return;
		}
		if (!DroneGPS::IsValid(Drone))
		{
			SetRightPanelText(
				StatusName,
				FString::Printf(TEXT("导出失败：无人机 %s 没有有效的3D GPS"), *DroneID));
			return;
		}
		const int32* DisplayID = DisplayIDsByDroneID.Find(DroneID);
		if (!DisplayID)
		{
			SetRightPanelText(
				StatusName,
				FString::Printf(TEXT("导出失败：无人机 %s 没有有效编号"), *DroneID));
			return;
		}

		const FVector RelativeWorld = DronePositions[Index] - OriginWorld;
		FGCSBenchmarkExportRow& Row = Rows.AddDefaulted_GetRef();
		Row.DisplayID = *DisplayID;
		Row.EastMeters = FVector::DotProduct(RelativeWorld, EastWorld) / 100.0;
		Row.NorthMeters = FVector::DotProduct(RelativeWorld, NorthWorld) / 100.0;
	}
	if (Fleet->GetFleetRevision() != SnapshotRevision)
	{
		SetRightPanelText(StatusName, TEXT("导出取消：无人机数据已变化，请重试"));
		return;
	}

	FString BenchmarkText;
	if (!GCSBenchmarkExport::BuildLegacyBenchmarkText(
		Rows,
		LegacyOriginDisplayID,
		GeoOrigin.OriginLatitude,
		GeoOrigin.OriginLongitude,
		BenchmarkText,
		Error))
	{
		SetRightPanelText(StatusName, FString::Printf(TEXT("导出失败：%s"), *Error));
		return;
	}

	FString SavePath;
	const ERuntimeSaveDialogResult DialogResult = OpenRuntimeBenchmarkSaveDialog(SavePath);
	if (DialogResult == ERuntimeSaveDialogResult::Cancelled)
	{
		SetRightPanelText(StatusName, TEXT("已取消导出"));
		return;
	}
	if (DialogResult != ERuntimeSaveDialogResult::Selected || SavePath.IsEmpty())
	{
		SetRightPanelText(StatusName, TEXT("导出失败：无法打开保存窗口"));
		return;
	}
	if (!FFileHelper::SaveStringToFile(
		BenchmarkText,
		*SavePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		SetRightPanelText(StatusName, TEXT("导出失败：无法写入目标文件"));
		return;
	}

	SetRightPanelText(
		StatusName,
		FString::Printf(
			TEXT("导出成功：%s（%d架）"),
			*FPaths::GetCleanFilename(SavePath),
			Rows.Num()));
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[GCSBenchmarkExport] Saved=%s Drones=%d Origin=%s Lat=%.7f Lon=%.7f Revision=%lld"),
		*SavePath,
		Rows.Num(),
		*GeoOrigin.OriginDroneID,
		GeoOrigin.OriginLatitude,
		GeoOrigin.OriginLongitude,
		static_cast<long long>(SnapshotRevision));
}

void UGCSUIBridgeSubsystem::HandleMapCheckStateChanged(const bool bIsChecked)
{
	if (!bUpdatingMapCheckBoxFromScene)
	{
		SetSceneMapVisible(bIsChecked);
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
	// A top-level UI opening must cancel any scene gesture that may have started
	// on the same mouse interaction.
	EndMarqueeCameraLock();
	bMarqueePointerDown = false;
	bMarqueeSelecting = false;
	bMarqueeLeftMouseWasDown = false;
	UpdateSelectionRectangleVisual(FVector2D::ZeroVector, FVector2D::ZeroVector, false);

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
		// The root canvas fills the viewport for layout only. It must not block
		// the toolbar or the authored controls inside BORDER_RouteImport.
		RootWidget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

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

	ImportWidget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
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

void UGCSUIBridgeSubsystem::ResetTimeControlPreparationState()
{
	bTimeControlCountdownActive = false;
	bTimeControlPerformanceActive = false;
	TimeControlPerformanceElapsedSeconds = 0.0;
	TimeControlPreviewDatasetGeneration = INDEX_NONE;
	TimeControlTargetDroneIDs.Reset();
	TimeControlScheduleSentDroneIDs.Reset();
	TimeControlFailedDroneIDs.Reset();
	PendingTimeControlCommandID.Reset();
	PendingTimeControlTargetCount = 0;
	PendingTimeControlResultCount = 0;
	PendingTimeControlSucceededCount = 0;
	PendingTimeControlCompletedDroneIDs.Reset();
}

bool UGCSUIBridgeSubsystem::BeginScheduledRoutePreview(FString& OutError)
{
	OutError.Reset();
	URoutePreviewSubsystem* RoutePreview =
		GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview || !RoutePreview->IsRouteLoaded())
	{
		OutError = TEXT("未加载NPZ航线，无法启动UE表演预览");
		return false;
	}
	if (RoutePreview->GetRouteDatasetGeneration() != TimeControlPreviewDatasetGeneration)
	{
		OutError = TEXT("授时后NPZ已更换，不能播放另一份航线");
		return false;
	}
	if (!RoutePreview->SetSampleRateHz(URoutePreviewSubsystem::ScheduledShowSampleRateHz, OutError))
	{
		return false;
	}

	// A scheduled show always represents a fresh run. The bottom transport may
	// have been used for inspection before scheduling, so explicitly restore
	// Frame 0 before starting instead of resuming its previous cursor.
	if (!RoutePreview->Stop(OutError))
	{
		return false;
	}
	if (!RoutePreview->Play(OutError))
	{
		return false;
	}

	bTimeControlPerformanceActive = true;
	TimeControlPerformanceElapsedSeconds = 0.0;
	SetRightPanelText(TEXT("TXT_TimeCountdownLabel"), TEXT("表演时间"));
	SetRightPanelText(TEXT("TXT_TimeCountdown"), TEXT("00:00:00"));
	RefreshRoutePlaybackUI();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[TimeControlUI] SHOW_PREVIEW START Frames=%d Duration=%.3fs"),
		RoutePreview->GetFrameCount(),
		RoutePreview->GetDurationSeconds());
	return true;
}

void UGCSUIBridgeSubsystem::RefreshTimeControlPreparationStatus(const FString& Prefix)
{
	SetRightPanelText(
		TEXT("TXT_TimeControlStatus"),
		FString::Printf(
			TEXT("%s  授时:%d 异常:%d/%d"),
			Prefix.IsEmpty() ? TEXT("授时准备") : *Prefix,
			TimeControlScheduleSentDroneIDs.Num(),
			TimeControlFailedDroneIDs.Num(),
			TimeControlTargetDroneIDs.Num()));
}

void UGCSUIBridgeSubsystem::TickTimeControlUI(const float DeltaSeconds)
{
	if (!bTimeControlCountdownActive && !bTimeControlPerformanceActive)
	{
		return;
	}

	TimeControlUIRefreshAccumulator += FMath::Max(0.0f, DeltaSeconds);
	if (TimeControlUIRefreshAccumulator < TimeControlUIRefreshIntervalSeconds)
	{
		return;
	}
	TimeControlUIRefreshAccumulator = FMath::Fmod(
		TimeControlUIRefreshAccumulator,
		TimeControlUIRefreshIntervalSeconds);

	if (bTimeControlPerformanceActive)
	{
		URoutePreviewSubsystem* RoutePreview =
			GetGameInstance()->GetSubsystem<URoutePreviewSubsystem>();
		if (!RoutePreview)
		{
			bTimeControlPerformanceActive = false;
			SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("UE表演预览异常：播放器不可用"));
			return;
		}

		const ERoutePreviewPlaybackState State = RoutePreview->GetPlaybackState();
		if (!RoutePreview->IsRouteLoaded()
			|| RoutePreview->GetRouteDatasetGeneration() != TimeControlPreviewDatasetGeneration
			|| State == ERoutePreviewPlaybackState::Stopped
			|| State == ERoutePreviewPlaybackState::Paused)
		{
			bTimeControlPerformanceActive = false;
			SetRightPanelText(TEXT("TXT_TimeControlStatus"), TEXT("UE预览已停止或被调整，已退出同步；此操作不控制真机"));
			return;
		}

		TimeControlPerformanceElapsedSeconds = RoutePreview->GetCurrentTime();
		SetRightPanelText(
			TEXT("TXT_TimeCountdown"),
			FormatTimeControlDuration(FMath::FloorToInt(TimeControlPerformanceElapsedSeconds)));

		if (State == ERoutePreviewPlaybackState::Completed)
		{
			bTimeControlPerformanceActive = false;
			TimeControlPerformanceElapsedSeconds = RoutePreview->GetDurationSeconds();
			SetRightPanelText(
				TEXT("TXT_TimeCountdown"),
				FormatTimeControlDuration(FMath::FloorToInt(TimeControlPerformanceElapsedSeconds)));
			RefreshTimeControlPreparationStatus(TEXT("NPZ计划播放完成，真机完成状态未确认"));
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[TimeControlUI] SHOW_PREVIEW COMPLETED Duration=%.3fs Frame=%d/%d"),
				TimeControlPerformanceElapsedSeconds,
				RoutePreview->GetCurrentFrame(),
				RoutePreview->GetFrameCount());
		}
		return;
	}

	const FTimespan Remaining = ScheduledTimeLocal - FDateTime::Now();
	const int32 TotalSeconds = FMath::Max(
		0,
		FMath::CeilToInt(Remaining.GetTotalSeconds()));
	SetRightPanelText(TEXT("TXT_TimeCountdownLabel"), TEXT("倒计时"));
	SetRightPanelText(
		TEXT("TXT_TimeCountdown"),
		FormatTimeControlDuration(TotalSeconds));

	if (TotalSeconds == 0)
	{
		bTimeControlCountdownActive = false;
		SetRightPanelText(TEXT("TXT_TimeCountdownLabel"), TEXT("表演时间"));
		SetRightPanelText(TEXT("TXT_TimeCountdown"), TEXT("00:00:00"));
		FString PreviewError;
		if (BeginScheduledRoutePreview(PreviewError))
		{
			RefreshTimeControlPreparationStatus(
				FString::Printf(
					TEXT("授时时刻 %02d:%02d:%02d 已到，UE按NPZ计划播放（非真机进度）"),
					ScheduledTimeLocal.GetHour(),
					ScheduledTimeLocal.GetMinute(),
					ScheduledTimeLocal.GetSecond()));
		}
		else
		{
			RefreshTimeControlPreparationStatus(
				FString::Printf(
					TEXT("授时时刻已到；UE预览未启动：%s"),
					*PreviewError));
			UE_LOG(LogTemp, Warning, TEXT("[TimeControlUI] SHOW_PREVIEW rejected: %s"), *PreviewError);
		}
	}
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

void UGCSUIBridgeSubsystem::UnbindFleetList()
{
	if (UListView* List = FleetListView.Get())
	{
		List->OnItemSelectionChanged().RemoveAll(this);
		List->OnEntryWidgetGenerated().RemoveAll(this);
		List->OnEntryWidgetReleased().RemoveAll(this);
	}
	if (UCheckBox* SelectAll = FleetSelectAllCheckBox.Get())
	{
		SelectAll->OnCheckStateChanged.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleFleetSelectAllCheckStateChanged);
	}
	for (UGCSFleetRowCheckBinding* Binding : FleetListCheckBindings)
	{
		if (Binding)
		{
			Binding->Shutdown();
		}
	}
	FleetListEntryBindings.Reset();
	FleetListCheckBindings.Reset();
	FleetListRowObjects.Reset();
	FleetListView.Reset();
	FleetSelectAllCheckBox.Reset();
	bUpdatingFleetList = false;
	LastRenderedFleetDetailDroneID.Reset();
}

void UGCSUIBridgeSubsystem::HandleFleetListEntryGenerated(UUserWidget& Entry)
{
	HandleFleetListEntryReleased(Entry);
	UListView* List = FleetListView.Get();
	UObject* RowObject = List ? List->GetListObjectFromEntry(Entry) : nullptr;
	UCheckBox* CheckBox = Entry.WidgetTree
		? Cast<UCheckBox>(Entry.WidgetTree->FindWidget(TEXT("CHK_FleetRowSelect")))
		: nullptr;
	UButton* DetailButton = Entry.WidgetTree
		? Cast<UButton>(Entry.WidgetTree->FindWidget(TEXT("BTN_FleetRowDetail")))
		: nullptr;
	if (!RowObject || !CheckBox || !DetailButton)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("WBP_GCS_FleetRow entry requires CHK_FleetRowSelect and BTN_FleetRowDetail."));
		return;
	}

	UGCSFleetRowCheckBinding* Binding = NewObject<UGCSFleetRowCheckBinding>(this);
	Binding->Initialize(this, RowObject, CheckBox, DetailButton, &Entry);
	const UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	Binding->RefreshFromRowObject(
		Fleet ? Fleet->GetCurrentSelectedDroneID() : FString());
	FleetListCheckBindings.Add(Binding);
	FleetListEntryBindings.Add(&Entry, Binding);
}

void UGCSUIBridgeSubsystem::HandleFleetListEntryReleased(UUserWidget& Entry)
{
	if (TWeakObjectPtr<UGCSFleetRowCheckBinding>* Existing =
		FleetListEntryBindings.Find(&Entry))
	{
		if (UGCSFleetRowCheckBinding* Binding = Existing->Get())
		{
			Binding->Shutdown();
			FleetListCheckBindings.RemoveSingleSwap(Binding);
		}
		FleetListEntryBindings.Remove(&Entry);
	}
}

void UGCSUIBridgeSubsystem::HandleFleetListCommandCheckChanged(
	UObject* RowObject,
	const bool bIsChecked)
{
	if (bUpdatingFleetList || !RowObject)
	{
		return;
	}

	FString DroneID;
	if (!GetFleetString(RowObject, TEXT("DroneID"), DroneID) || DroneID.IsEmpty())
	{
		return;
	}
	SetCommandDroneSelected(DroneID, bIsChecked);
}

void UGCSUIBridgeSubsystem::HandleFleetListDetailClicked(UObject* RowObject)
{
	if (!RowObject)
	{
		return;
	}

	FString DroneID;
	if (!GetFleetString(RowObject, TEXT("DroneID"), DroneID) || DroneID.IsEmpty())
	{
		return;
	}

	UFleetManagerSubsystem* Fleet = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (!Fleet || !Fleet->SelectDrone(DroneID))
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("GCS ListView detail button rejected: DroneID=%s"),
			*DroneID);
	}
}

void UGCSUIBridgeSubsystem::BindFleetPanelButtons()
{
	UnbindFleetList();
	UUserWidget* FleetPanel = FleetPanelWidget.Get();
	UListView* List = FleetPanel && FleetPanel->WidgetTree
		? Cast<UListView>(FleetPanel->WidgetTree->FindWidget(TEXT("LV_FleetDrones")))
		: nullptr;
	if (List)
	{
		FleetListView = List;
		// ListView itself is display/scroll virtualization only. Explicit child
		// controls own both interactions, preventing checkbox clicks from also
		// changing the single-drone detail selection.
		List->SetSelectionMode(ESelectionMode::None);
		List->OnEntryWidgetGenerated().AddUObject(
			this,
			&UGCSUIBridgeSubsystem::HandleFleetListEntryGenerated);
		List->OnEntryWidgetReleased().AddUObject(
			this,
			&UGCSUIBridgeSubsystem::HandleFleetListEntryReleased);

		FleetSelectAllCheckBox = FleetPanel->WidgetTree
			? Cast<UCheckBox>(FleetPanel->WidgetTree->FindWidget(TEXT("CHK_FleetSelectAll")))
			: nullptr;
		if (UCheckBox* SelectAll = FleetSelectAllCheckBox.Get())
		{
			SelectAll->OnCheckStateChanged.AddUniqueDynamic(
				this,
				&UGCSUIBridgeSubsystem::HandleFleetSelectAllCheckStateChanged);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Fleet ListView panel missing CHK_FleetSelectAll."));
		}

		UE_LOG(LogTemp, Log, TEXT("[GCS_FLEET_LIST] LV_FleetDrones bound."));
		return;
	}

	InitializeFleetRowViews();

	if (UButton* Button = FindFleetButton(TEXT("BTN_FleetSelectALL")))
	{
		Button->OnClicked.RemoveDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleFleetSelectAllClicked);
		Button->OnClicked.AddDynamic(
			this,
			&UGCSUIBridgeSubsystem::HandleFleetSelectAllClicked);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("FleetPanel missing BTN_FleetSelectALL."));
	}
	if (!FindFleetText(TEXT("BTN_FleetSelectALL_Text")))
	{
		UE_LOG(LogTemp, Error, TEXT("FleetPanel missing BTN_FleetSelectALL_Text."));
	}

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

void UGCSUIBridgeSubsystem::BindTelemetryFilterToggle()
{
	if (UButton* Previous = TelemetryFilterToggleButton.Get())
		Previous->OnClicked.RemoveDynamic(this, &ThisClass::HandleTelemetryFilterToggleClicked);
	TelemetryFilterToggleButton = FGCSUIWidgetPresenter::FindButton(RightToolbarWidget.Get(), TEXT("BTN_ShaiXuan"));
	if (UButton* Button = TelemetryFilterToggleButton.Get())
		Button->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleTelemetryFilterToggleClicked);
	else
		UE_LOG(LogTemp, Warning, TEXT("Telemetry filter toggle BTN_ShaiXuan is missing from the saved right toolbar."));
}

void UGCSUIBridgeSubsystem::HandleTelemetryFilterToggleClicked()
{
	UUserWidget* Filter = TelemetryFilterWidget.Get();
	if (!Filter)
	{
		UUserWidget* Main = MainWidgetRef.Get();
		if (!Main) return;
		UClass* FilterClass = LoadClass<UUserWidget>(nullptr,
			TEXT("/Game/UI/WBP_GCS_TelemetryFilter.WBP_GCS_TelemetryFilter_C"));
		if (!FilterClass) return;
		Filter = CreateWidget<UUserWidget>(Main, FilterClass);
		if (!Filter) return;
		TelemetryFilterController = NewObject<UGCSTelemetryFilterController>(this);
		if (!TelemetryFilterController->Bind(Filter, this))
		{
			TelemetryFilterController = nullptr;
			return;
		}
		TelemetryFilterWidget = Filter;
		// Independent viewport layer above navigation pages (95), not a child
		// of Logs. The bounded 300x820 panel leaves the toolbar/map outside it interactive.
		Filter->SetVisibility(ESlateVisibility::Collapsed);
		Filter->AddToViewport(96);
		Filter->SetAlignmentInViewport(FVector2D(1, 0));
		Filter->SetDesiredSizeInViewport(FVector2D(300, 820));
		Filter->SetPositionInViewport(FVector2D(-400, 30), false);
		// UE's size/position setters reset anchors; set the right anchor last.
		Filter->SetAnchorsInViewport(FAnchors(1, 0));
	}
	const bool bShow = !Filter->IsVisible();
	Filter->SetVisibility(bShow ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (bShow) RefreshTelemetryTableFromProduction();
}

void UGCSUIBridgeSubsystem::ResetTelemetryFilter()
{
	if (UButton* Button = TelemetryFilterToggleButton.Get())
		Button->OnClicked.RemoveDynamic(this, &ThisClass::HandleTelemetryFilterToggleClicked);
	TelemetryFilterToggleButton.Reset();
	if (UUserWidget* Filter = TelemetryFilterWidget.Get()) Filter->RemoveFromParent();
	TelemetryFilterWidget.Reset();
	if (TelemetryFilterController) TelemetryFilterController->Shutdown();
	TelemetryFilterController = nullptr;
}

void UGCSUIBridgeSubsystem::CreateNavigationPages()
{
	MapPageWidget = CreateNavigationPage(
		TEXT("/Game/UI/WBP_GCS_PageMap.WBP_GCS_PageMap_C"));
	LogsPageWidget = CreateNavigationPage(
		TEXT("/Game/UI/WBP_GCS_PageLogs.WBP_GCS_PageLogs_C"));
	ConfigureTelemetryTableInput();
	TelemetryTableRefreshAccumulator = TelemetryTableRefreshIntervalSeconds;
}

void UGCSUIBridgeSubsystem::ConfigureGroundStationInputPolicy(UUserWidget* MainWidget) const
{
	APlayerController* PlayerController = MainWidget ? MainWidget->GetOwningPlayer() : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogTemp, Warning, TEXT("GCS input policy skipped: no owning PlayerController."));
		return;
	}

	PlayerController->bShowMouseCursor = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);

	if (UWorld* World = PlayerController->GetWorld())
	{
		if (UGameViewportClient* ViewportClient = World->GetGameViewport())
		{
			// Marquee selection and top-down panning poll left/middle button state
			// through the PlayerController, so all mouse buttons must update game
			// input. Camera movement is gated separately by button and UI geometry.
			ViewportClient->SetMouseCaptureMode(EMouseCaptureMode::CaptureDuringMouseDown);
			ViewportClient->SetMouseLockMode(EMouseLockMode::DoNotLock);
		}
	}
}

void UGCSUIBridgeSubsystem::ConfigureTelemetryTableInput() const
{
	UUserWidget* LogsPage = LogsPageWidget.Get();
	if (!LogsPage || !LogsPage->WidgetTree)
	{
		return;
	}

	UScrollBox* HorizontalScroll = Cast<UScrollBox>(
		LogsPage->WidgetTree->FindWidget(TEXT("SB_Table_H")));
	UListView* TelemetryList = Cast<UListView>(
		LogsPage->WidgetTree->FindWidget(TEXT("LV_Telemetry")));

	if (!HorizontalScroll || !TelemetryList)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("Telemetry table input setup failed: SB_Table_H or LV_Telemetry is missing."));
		return;
	}

	HorizontalScroll->SetOrientation(Orient_Horizontal);
	HorizontalScroll->SetConsumeMouseWheel(EConsumeMouseWheel::Never);
	HorizontalScroll->SetAllowRightClickDragScrolling(false);
	HorizontalScroll->SetScrollBarVisibility(ESlateVisibility::Visible);

	if (TelemetryList->GetOrientation() != Orient_Vertical)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("LV_Telemetry must remain vertical so it owns the mouse wheel."));
	}
}

void UGCSUIBridgeSubsystem::UpdateTelemetryHorizontalScrollInput(
	APlayerController* PlayerController)
{
	UUserWidget* LogsPage = LogsPageWidget.Get();
	UScrollBox* HorizontalScroll = LogsPage && LogsPage->IsVisible() && LogsPage->WidgetTree
		? Cast<UScrollBox>(LogsPage->WidgetTree->FindWidget(TEXT("SB_Table_H")))
		: nullptr;
	const bool bRightMouseDown = PlayerController
		&& PlayerController->IsInputKeyDown(EKeys::RightMouseButton);
	const FVector2D ScreenPosition = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetCursorPos()
		: FVector2D::ZeroVector;

	if (bRightMouseDown && !bTelemetryRightMouseWasDown)
	{
		bTelemetryRightMouseWasDown = true;
		if (IsVisibleWidgetUnderLocation(HorizontalScroll, ScreenPosition))
		{
			bTelemetryHorizontalDragging = true;
			TelemetryHorizontalDragLastLocalPosition =
				HorizontalScroll->GetCachedGeometry().AbsoluteToLocal(ScreenPosition);
		}
	}

	if (bRightMouseDown && bTelemetryHorizontalDragging)
	{
		if (!HorizontalScroll)
		{
			ResetTelemetryHorizontalDrag();
			return;
		}

		const FVector2D CurrentLocalPosition =
			HorizontalScroll->GetCachedGeometry().AbsoluteToLocal(ScreenPosition);
		const float HorizontalDelta =
			CurrentLocalPosition.X - TelemetryHorizontalDragLastLocalPosition.X;
		if (!FMath::IsNearlyZero(HorizontalDelta))
		{
			HorizontalScroll->SetScrollOffset(FMath::Clamp(
				HorizontalScroll->GetScrollOffset() - HorizontalDelta,
				0.0f,
				HorizontalScroll->GetScrollOffsetOfEnd()));
		}
		TelemetryHorizontalDragLastLocalPosition = CurrentLocalPosition;
	}

	if (!bRightMouseDown && bTelemetryRightMouseWasDown)
	{
		ResetTelemetryHorizontalDrag();
	}
}

void UGCSUIBridgeSubsystem::ResetTelemetryHorizontalDrag()
{
	bTelemetryRightMouseWasDown = false;
	bTelemetryHorizontalDragging = false;
	TelemetryHorizontalDragLastLocalPosition = FVector2D::ZeroVector;
}

void UGCSUIBridgeSubsystem::TickTelemetryTableUI(const float DeltaSeconds)
{
	TelemetryTableRefreshAccumulator += DeltaSeconds;
	if (TelemetryTableRefreshAccumulator < TelemetryTableRefreshIntervalSeconds)
	{
		return;
	}
	TelemetryTableRefreshAccumulator = 0.0f;
	RefreshTelemetryTableFromProduction();
}

void UGCSUIBridgeSubsystem::RefreshTelemetryTableFromProduction()
{
	// The filter always reads production data, including when the table is in
	// authored mock-preview mode. Never feed designer sample rows into results.
	if (TelemetryFilterController && GetGameInstance())
	{
		if (auto* Source = GetGameInstance()->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>())
		{
			TMap<FString, bool> Online;
			const auto SourceRows = Source->GetMDHKTelemetryRows();
			if (auto* Fleet = GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>())
				for (const auto& Row : SourceRows)
				{
					FDroneData Drone;
					if (Fleet->GetDroneByID(Row.DroneID, Drone)) Online.Add(Row.DroneID, Drone.bOnline);
				}
			const FDateTime Now = FDateTime::UtcNow();
			TelemetryFilterController->UpdateTelemetry(SourceRows, Online, Source->IsTelemetryLinkConnected(),
				Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond(), Source->GetTelemetryTimeoutMs());
		}
	}
	UUserWidget* LogsPage = LogsPageWidget.Get();
	if (!LogsPage || !LogsPage->WidgetTree)
	{
		return;
	}

	const FBoolProperty* MockProperty =
		FindFProperty<FBoolProperty>(LogsPage->GetClass(), TEXT("bUseMockTelemetry"));
	if (!MockProperty)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Telemetry table requires the WBP_GCS_PageLogs bUseMockTelemetry boolean."));
		return;
	}
	if (MockProperty->GetPropertyValue_InContainer(LogsPage))
	{
		return;
	}

	UListView* TelemetryList = Cast<UListView>(
		LogsPage->WidgetTree->FindWidget(TEXT("LV_Telemetry")));
	UDSPGCSProductionTelemetryProviderSubsystem* Provider = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>()
		: nullptr;
	if (!TelemetryList || !Provider)
	{
		return;
	}

	TArray<FMDHKTelemetryTableRow> Rows = Provider->GetMDHKTelemetryRows();
	Rows.Sort([](const FMDHKTelemetryTableRow& Left, const FMDHKTelemetryTableRow& Right)
	{
		const int32 LeftNumber = ParseLogicalDroneNumber(Left.DroneID);
		const int32 RightNumber = ParseLogicalDroneNumber(Right.DroneID);
		if (LeftNumber != INDEX_NONE && RightNumber != INDEX_NONE && LeftNumber != RightNumber)
		{
			return LeftNumber < RightNumber;
		}
		return Left.DroneID < Right.DroneID;
	});

	bool bRebuildItems = TelemetryRowObjects.Num() != Rows.Num()
		|| TelemetryList->GetNumItems() != Rows.Num();
	if (!bRebuildItems)
	{
		for (const FMDHKTelemetryTableRow& Row : Rows)
		{
			if (!TelemetryRowObjects.Contains(Row.DroneID))
			{
				bRebuildItems = true;
				break;
			}
		}
	}

	if (bRebuildItems)
	{
		TelemetryList->ClearListItems();
		TelemetryRowObjects.Reset();
	}

	UClass* RowDataClass = LoadObject<UClass>(
		nullptr,
		TEXT("/Game/UI/BP_TelemetryRowData.BP_TelemetryRowData_C"));
	if (!RowDataClass)
	{
		UE_LOG(LogTemp, Error, TEXT("BP_TelemetryRowData class could not be loaded."));
		return;
	}

	for (const FMDHKTelemetryTableRow& Row : Rows)
	{
		TObjectPtr<UObject>* Existing = TelemetryRowObjects.Find(Row.DroneID);
		UObject* RowObject = Existing ? Existing->Get() : nullptr;
		if (!RowObject)
		{
			RowObject = NewObject<UObject>(this, RowDataClass);
			TelemetryRowObjects.Add(Row.DroneID, RowObject);
			TelemetryList->AddItem(RowObject);
		}

		const int32 LogicalID = ParseLogicalDroneNumber(Row.DroneID);
		const bool bPropertiesValid =
			SetTelemetryInteger(RowObject, TEXT("UavID"), LogicalID) &&
			SetTelemetryInteger(RowObject, TEXT("Battery"),
				Row.bBatteryRemainingValid ? Row.BatteryRemainingPercent : -1) &&
			SetTelemetryReal(RowObject, TEXT("Temperature"),
				Row.bBatteryTemperatureValid ? Row.BatteryTemperatureCelsius : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell1"), Row.bCell1Valid ? Row.Cell1Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell2"), Row.bCell2Valid ? Row.Cell2Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell3"), Row.bCell3Valid ? Row.Cell3Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell4"), Row.bCell4Valid ? Row.Cell4Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Latitude"), Row.bGPSValid ? Row.LatitudeDeg : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Longitude"), Row.bGPSValid ? Row.LongitudeDeg : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Altitude"),
				Row.bAltitudeValid ? Row.AltitudeAmslMeters : 0.0) &&
			SetTelemetryInteger64(RowObject, TEXT("TimeBootMs"), Row.TimeBootMs) &&
			SetTelemetryString(RowObject, TEXT("GPSUtc"), Row.GPSUtc) &&
			SetTelemetryString(RowObject, TEXT("ValidFlags"), Row.ValidFlagsText) &&
			SetTelemetryReal(RowObject, TEXT("RelativeAltitude"), Row.RelativeAltitudeMeters) &&
			SetTelemetryInteger64(RowObject, TEXT("FreeMemoryBytes"), Row.FreeMemoryBytes) &&
			SetTelemetryString(RowObject, TEXT("FlightSWVersion"), Row.FlightSoftwareVersionText) &&
			SetTelemetryReal(RowObject, TEXT("WaypointDistance"), Row.WaypointDistanceMeters) &&
			SetTelemetryReal(RowObject, TEXT("CompassVariance"), Row.CompassVariance) &&
			SetTelemetryReal(RowObject, TEXT("MagField1"), Row.MagneticField1Mgauss) &&
			SetTelemetryReal(RowObject, TEXT("MagField2"), Row.MagneticField2Mgauss) &&
			SetTelemetryInteger(RowObject, TEXT("BatteryCycleCount"), Row.BatteryCycleCount) &&
			SetTelemetryInteger(RowObject, TEXT("CellVoltageDeltaMv"), Row.CellVoltageDeltaMv) &&
			SetTelemetryString(RowObject, TEXT("EKFFlags"), Row.EKFFlagsText) &&
			SetTelemetryInteger(RowObject, TEXT("MissionSeq"), Row.MissionSequence) &&
			SetTelemetryInteger(RowObject, TEXT("MissionTotal"), Row.MissionTotal) &&
			SetTelemetryReal(RowObject, TEXT("BatteryCurrent"), Row.BatteryCurrentAmps) &&
			SetTelemetryInteger(RowObject, TEXT("GPSFixType"), Row.GPSFixType) &&
			SetTelemetryInteger(RowObject, TEXT("SatellitesVisible"), Row.SatellitesVisible) &&
			SetTelemetryInteger(RowObject, TEXT("PayloadSchemaVersion"), Row.PayloadSchemaVersion) &&
			SetTelemetryString(RowObject, TEXT("RGB"), Row.RGBText) &&
			SetTelemetryString(RowObject, TEXT("FlightCustomVersion"), Row.FlightCustomVersionText) &&
			SetTelemetryString(RowObject, TEXT("HardwareID"), Row.HardwareID);
		if (!bPropertiesValid)
		{
			UE_LOG(LogTemp, Error,
				TEXT("BP_TelemetryRowData no longer matches the expanded 32-column production contract."));
			return;
		}
	}

	// RequestRefresh only reconciles added/removed items. These row objects keep
	// their identity while their telemetry properties change, so force entry
	// re-binding to execute WBP_TelemetryRow::OnListItemObjectSet again.
	TelemetryList->RegenerateAllEntries();
}

void UGCSUIBridgeSubsystem::ResetNavigationPages()
{
	ResetTelemetryHorizontalDrag();
	ResetTelemetryFilter();
	for (TWeakObjectPtr<UUserWidget>* Page :
		{ &MapPageWidget, &LogsPageWidget })
	{
		if (UUserWidget* Widget = Page->Get())
		{
			Widget->RemoveFromParent();
		}

		Page->Reset();
	}
}

void UGCSUIBridgeSubsystem::ApplyNavigationVisibility()
{
	const bool bShowFleet =
		bPanelsExpanded && bFleetPanelRequestedVisible;
	const bool bShowDroneDetail =
		bPanelsExpanded && bDroneDetailRequestedVisible;

	FGCSUIWidgetPresenter::SetWidgetVisible(FleetPanelWidget, bShowFleet);
	FGCSUIWidgetPresenter::SetWidgetVisible(DroneDetailWidget, bShowDroneDetail);

	UE_LOG(
		LogTemp,
		Log,
		TEXT("GCS primary panels: expanded=%s fleet=%s control=%s"),
		bPanelsExpanded ? TEXT("true") : TEXT("false"),
		bShowFleet ? TEXT("visible") : TEXT("collapsed"),
		bShowDroneDetail ? TEXT("visible") : TEXT("collapsed"));
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

#include "GCSRouteWorkflowCoordinator.inl"
