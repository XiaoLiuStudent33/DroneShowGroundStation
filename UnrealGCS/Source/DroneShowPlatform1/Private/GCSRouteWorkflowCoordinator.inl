#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "DSPGCSUploadJobSubsystem.h"
#include "DroneLogicalId.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "GCSUIBridgeSubsystem.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceSubsystem.h"
#include "RouteAssignmentSubsystem.h"
#include "RoutePreviewSubsystem.h"
#include "RouteWaypointGenerationSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogGCSRouteWorkflow, Log, All);

namespace GCSRouteWorkflowPrivate
{
	template <typename T>
	T* Get(const TMap<FName, TWeakObjectPtr<UWidget>>& Map, const FName Name)
	{
		const TWeakObjectPtr<UWidget>* Value = Map.Find(Name);
		return Value ? Cast<T>(Value->Get()) : nullptr;
	}

	void SetText(const TMap<FName, TWeakObjectPtr<UWidget>>& Map, const FName Name, const FString& Value)
	{
		if (UTextBlock* Widget = Get<UTextBlock>(Map, Name)) Widget->SetText(FText::FromString(Value));
	}

	void SetEdit(const TMap<FName, TWeakObjectPtr<UWidget>>& Map, const FName Name, const FString& Value)
	{
		if (UEditableTextBox* Widget = Get<UEditableTextBox>(Map, Name)) Widget->SetText(FText::FromString(Value));
	}

	void SetTooltip(const TMap<FName, TWeakObjectPtr<UWidget>>& Map, const FName Name, const FString& Value)
	{
		if (UWidget* Widget = Get<UWidget>(Map, Name)) Widget->SetToolTipText(FText::FromString(Value));
	}

	FString WorkflowFromUtf8(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	FString WorkflowDigestHex(const dspgcs::UploadJobDigest& Digest)
	{
		static constexpr TCHAR Digits[] = TEXT("0123456789abcdef");
		FString Result;
		Result.Reserve(64);
		for (const uint8 Byte : Digest)
		{
			Result.AppendChar(Digits[(Byte >> 4) & 0x0f]);
			Result.AppendChar(Digits[Byte & 0x0f]);
		}
		return Result;
	}

	bool WorkflowParseFiniteDouble(const FString& Text, double& OutValue)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		return !Trimmed.IsEmpty()
			&& Trimmed.IsNumeric()
			&& LexTryParseString(OutValue, *Trimmed)
			&& FMath::IsFinite(OutValue);
	}

	TArray<TPair<FName, UClass*>> Required(std::initializer_list<const TCHAR*> Names)
	{
		TArray<TPair<FName, UClass*>> Result;
		for (const TCHAR* Name : Names)
		{
			const FString Value(Name);
			UClass* Type = UTextBlock::StaticClass();
			if (Value.StartsWith(TEXT("BTN_"))) Type = UButton::StaticClass();
			else if (Value.StartsWith(TEXT("EDT_"))) Type = UEditableTextBox::StaticClass();
			else if (Value.StartsWith(TEXT("PROGRESS_"))) Type = UProgressBar::StaticClass();
			else if (Value.StartsWith(TEXT("SWITCHER_"))) Type = UWidgetSwitcher::StaticClass();
			else if (Value.StartsWith(TEXT("LIST_"))) Type = UListView::StaticClass();
			Result.Emplace(FName(Name), Type);
		}
		return Result;
	}

	FString Current(const ERouteArtifactCurrentStatus V)
	{
		return V == ERouteArtifactCurrentStatus::Ready ? TEXT("Ready")
			: (V == ERouteArtifactCurrentStatus::LegacyUnsupported ? TEXT("Legacy v1")
				: (V == ERouteArtifactCurrentStatus::Invalid ? TEXT("Invalid") : TEXT("None")));
	}
	FString Generation(const ERouteArtifactGenerationStatus V)
	{
		switch (V) { case ERouteArtifactGenerationStatus::Creating:return TEXT("Creating"); case ERouteArtifactGenerationStatus::Generating:return TEXT("Generating"); case ERouteArtifactGenerationStatus::Validating:return TEXT("Validating"); case ERouteArtifactGenerationStatus::Finalizing:return TEXT("Finalizing"); case ERouteArtifactGenerationStatus::Failed:return TEXT("Failed"); default:return TEXT("None"); }
	}
	FString Recovery(const ERouteArtifactRecoveryStatus V)
	{
		switch (V) { case ERouteArtifactRecoveryStatus::Recovered:return TEXT("Recovered"); case ERouteArtifactRecoveryStatus::RecoveryRequired:return TEXT("Recovery Required"); case ERouteArtifactRecoveryStatus::Unrecoverable:return TEXT("Unrecoverable"); case ERouteArtifactRecoveryStatus::Busy:return TEXT("Busy"); default:return TEXT("Stable"); }
	}
	FString Ready(const ERouteArtifactCurrentStatus C, const ERouteArtifactRecoveryStatus R)
	{
		if (R == ERouteArtifactRecoveryStatus::RecoveryRequired || R == ERouteArtifactRecoveryStatus::Unrecoverable) return TEXT("RECOVERY REQUIRED");
		return C == ERouteArtifactCurrentStatus::Ready ? TEXT("READY")
			: (C == ERouteArtifactCurrentStatus::LegacyUnsupported ? TEXT("LEGACY V1")
				: (C == ERouteArtifactCurrentStatus::Invalid ? TEXT("INVALID") : TEXT("NOT READY")));
	}
	FString JobState(const dspgcs::UploadJobState V)
	{
		switch (V) { case dspgcs::UploadJobState::Created:return TEXT("Created"); case dspgcs::UploadJobState::Validating:return TEXT("Validating"); case dspgcs::UploadJobState::Ready:return TEXT("Ready"); case dspgcs::UploadJobState::Running:return TEXT("Running"); case dspgcs::UploadJobState::Paused:return TEXT("Paused"); case dspgcs::UploadJobState::Cancelling:return TEXT("Cancelling"); case dspgcs::UploadJobState::Completed:return TEXT("Succeeded"); case dspgcs::UploadJobState::Failed:return TEXT("Failed"); case dspgcs::UploadJobState::Cancelled:return TEXT("Cancelled"); default:return TEXT("--"); }
	}
	FString Gate(const dspgcs::UploadArtifactGateState V)
	{
		switch (V) { case dspgcs::UploadArtifactGateState::NotChecked:return TEXT("Not Checked"); case dspgcs::UploadArtifactGateState::Validating:return TEXT("Validating"); case dspgcs::UploadArtifactGateState::Validated:return TEXT("Validated"); case dspgcs::UploadArtifactGateState::Rejected:return TEXT("Rejected"); default:return TEXT("--"); }
	}
	FString FrozenState(const EDSPGCSFrozenAssignmentSyncState V)
	{
		switch (V)
		{
		case EDSPGCSFrozenAssignmentSyncState::NotFrozen:return TEXT("Frozen: Not Frozen");
		case EDSPGCSFrozenAssignmentSyncState::FrozenLocal:return TEXT("Frozen: Local Only");
		case EDSPGCSFrozenAssignmentSyncState::SyncingToConsole:return TEXT("Frozen: Syncing");
		case EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted:return TEXT("Frozen: Console Accepted");
		case EDSPGCSFrozenAssignmentSyncState::SyncFailed:return TEXT("Frozen: Sync Failed");
		default:return TEXT("Frozen: --");
		}
	}
	FString Playback(const ERoutePreviewPlaybackState V)
	{
		switch (V) { case ERoutePreviewPlaybackState::Playing:return TEXT("Playing"); case ERoutePreviewPlaybackState::Paused:return TEXT("Paused"); case ERoutePreviewPlaybackState::Stopped:return TEXT("Stopped"); default:return TEXT("--"); }
	}
	FString Compact(const double Value)
	{
		FString Result = FString::Printf(TEXT("%.1f"), Value);
		if (Result.EndsWith(TEXT(".0"))) Result.LeftChopInline(2);
		return Result;
	}

	bool SameAssignmentSource(
		const FRouteAssignmentSourceIdentity& Left,
		const FRouteAssignmentSourceIdentity& Right)
	{
		return Left.SourceDatasetDigest == Right.SourceDatasetDigest
			&& Left.NormalizedShowOriginLatitudeE7 == Right.NormalizedShowOriginLatitudeE7
			&& Left.NormalizedShowOriginLongitudeE7 == Right.NormalizedShowOriginLongitudeE7
			&& Left.FleetSessionID == Right.FleetSessionID
			&& Left.FleetRevisionAtCapture == Right.FleetRevisionAtCapture
			&& Left.RouteDatasetGeneration == Right.RouteDatasetGeneration
			&& Left.bShowOriginSet == Right.bShowOriginSet;
	}

	bool SameAutoStartTarget(
		const FGCSRouteAutoStartTargetSnapshot& Left,
		const FGCSRouteAutoStartTargetSnapshot& Right)
	{
		return Left.DroneID == Right.DroneID
			&& Left.EndpointIdentity == Right.EndpointIdentity
			&& Left.NumericDroneID == Right.NumericDroneID
			&& Left.RouteSlotIndex == Right.RouteSlotIndex
			&& Left.RouteRelativePath == Right.RouteRelativePath
			&& Left.RouteFileByteSize == Right.RouteFileByteSize
			&& Left.RouteDigest == Right.RouteDigest;
	}
}

bool UGCSRouteWorkflowCoordinator::Initialize(UGameInstance* GI, UUserWidget* RightPanel)
{
	Shutdown();
	if (!IsInGameThread() || !GI || !RightPanel || !RightPanel->WidgetTree) return false;
	UButton* Button = Cast<UButton>(RightPanel->WidgetTree->FindWidget(TEXT("BTN_ConsoleRouteUpload_1")));
	if (!Button)
	{
		UE_LOG(LogGCSRouteWorkflow, Error, TEXT("WBP_GCS_RightPanel is missing BTN_ConsoleRouteUpload_1."));
		return false;
	}
	GameInstanceRef = GI;
	RightPanelWidgetRef = RightPanel;
	OpenWorkflowButtonRef = Button;
	Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteWorkflowClicked);
	Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteWorkflowClicked);
	return true;
}

void UGCSRouteWorkflowCoordinator::Shutdown()
{
	StopRefreshTicker();
	UnbindCommandControls();
	ClearRouteListModel();
	if (UButton* Button = OpenWorkflowButtonRef.Get()) Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteWorkflowClicked);
	if (RouteWorkflowWidget) RouteWorkflowWidget->RemoveFromParent();
	RouteWorkflowWidget = nullptr;
	RouteConvertWidgetRef.Reset(); RouteUploadWidgetRef.Reset();
	WorkflowWidgets.Reset(); ConvertWidgets.Reset(); UploadWidgets.Reset();
	OpenWorkflowButtonRef.Reset(); RightPanelWidgetRef.Reset(); GameInstanceRef.Reset();
	bBindingValid = false;
	bUploadArtifactMismatch = false;
	SelectedRouteImportFilePath.Reset();
	SelectedRouteSourceDigest.Reset();
	CommandErrorMessage.Reset();
	CommandErrorExpiresAtSeconds = 0.0;
	CommandErrorCategory = EGCSRouteWorkflowCommandErrorCategory::None;
	bCommandInFlight = false;
	bViewVisible = false;
	SelectedPageIndex = 0;
	AutoStartState = EGCSRouteAutoStartState::Idle;
	AutoStartSnapshot = FGCSRouteAutoStartSnapshot{};
	AutoStartDiagnostic.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	WorkflowFactoryForTest = {};
	bSkipRequiredWidgetBindingForTest = false;
	CommandTestHooks = {};
	AutoStartTestHooks = {};
	CommandGateInputsForTest.Reset();
	CloseRequestCountForTest = 0;
#endif
}

void UGCSRouteWorkflowCoordinator::HandleRouteWorkflowClicked()
{
	FString Error;
	if (!ShowRouteWorkflow(Error)) UE_LOG(LogGCSRouteWorkflow, Error, TEXT("Unable to show Route Workflow: %s"), *Error);
}

bool UGCSRouteWorkflowCoordinator::ShowRouteWorkflow(FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) { OutError = TEXT("Coordinator not initialized on Game Thread."); return false; }
	if (!RouteWorkflowWidget)
	{
		RouteWorkflowWidget = CreateRouteWorkflowWidget(OutError);
		if (!RouteWorkflowWidget) return false;
#if WITH_DEV_AUTOMATION_TESTS
		++WorkflowCreationCount;
#endif
	}
	if (!bBindingValid)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (bSkipRequiredWidgetBindingForTest) bBindingValid = true; else
#endif
		if (!BindWorkflowWidgets(RouteWorkflowWidget, OutError))
		{
			RouteWorkflowWidget->SetVisibility(ESlateVisibility::Hidden);
			StopRefreshTicker();
			return false;
		}
	}
#if WITH_DEV_AUTOMATION_TESTS
	const bool bTest = static_cast<bool>(WorkflowFactoryForTest);
#else
	const bool bTest = false;
#endif
	if (!bTest && !RouteWorkflowWidget->IsInViewport()) RouteWorkflowWidget->AddToViewport(100);
	SelectRoutePage(SelectedPageIndex);
	if (RouteWorkflowWidget->WidgetTree)
	{
		if (UWidget* AuthoredRoot = RouteWorkflowWidget->WidgetTree->FindWidget(
			TEXT("BORDER_RouteWorkflow")))
		{
			AuthoredRoot->SetVisibility(ESlateVisibility::Visible);
		}
	}
	RouteWorkflowWidget->SetVisibility(ESlateVisibility::Visible);
	bViewVisible = true;
	RefreshReadOnlyState();
#if WITH_DEV_AUTOMATION_TESTS
	if (!bTest)
#endif
	if (UDSPGCSUploadJobSubsystem* Upload = GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>()) Upload->NotifyUploadPageOpened();
	StartRefreshTicker();
	return true;
}

void UGCSRouteWorkflowCoordinator::HandleRouteCloseClicked()
{
	if (!IsInGameThread() || !RouteWorkflowWidget) return;
	if (UWidgetSwitcher* Switcher = GCSRouteWorkflowPrivate::Get<UWidgetSwitcher>(
		WorkflowWidgets, TEXT("SWITCHER_RoutePages")))
	{
		SelectedPageIndex = Switcher->GetActiveWidgetIndex();
	}
	if (RouteWorkflowWidget->WidgetTree)
	{
		if (UWidget* AuthoredRoot = RouteWorkflowWidget->WidgetTree->FindWidget(
			TEXT("BORDER_RouteWorkflow")))
		{
			AuthoredRoot->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	RouteWorkflowWidget->SetVisibility(ESlateVisibility::Collapsed);
	bViewVisible = false;
#if WITH_DEV_AUTOMATION_TESTS
	++CloseRequestCountForTest;
#endif
}

void UGCSRouteWorkflowCoordinator::HandleRouteConvertTabClicked()
{
	SelectRoutePage(0);
}

void UGCSRouteWorkflowCoordinator::HandleRouteUploadTabClicked()
{
	SelectRoutePage(1);
}

void UGCSRouteWorkflowCoordinator::SelectRoutePage(const int32 PageIndex)
{
	if (UWidgetSwitcher* Switcher = GCSRouteWorkflowPrivate::Get<UWidgetSwitcher>(
		WorkflowWidgets, TEXT("SWITCHER_RoutePages")))
	{
		if (Switcher->GetNumWidgets() > 0)
		{
			SelectedPageIndex = FMath::Clamp(PageIndex, 0, Switcher->GetNumWidgets() - 1);
			Switcher->SetActiveWidgetIndex(SelectedPageIndex);
		}
	}
}

UUserWidget* UGCSRouteWorkflowCoordinator::CreateRouteWorkflowWidget(FString& OutError)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (WorkflowFactoryForTest)
	{
		UUserWidget* Result = WorkflowFactoryForTest();
		if (!Result) OutError = TEXT("Test factory returned null.");
		return Result;
	}
#endif
	UClass* Class = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_GCS_RouteWorkflow.WBP_GCS_RouteWorkflow_C"));
	if (!Class) { OutError = TEXT("Unable to load WBP_GCS_RouteWorkflow."); return nullptr; }
	APlayerController* Player = RightPanelWidgetRef.IsValid() ? RightPanelWidgetRef->GetOwningPlayer() : nullptr;
	UUserWidget* Result = Player ? CreateWidget<UUserWidget>(Player, Class) : CreateWidget<UUserWidget>(GameInstanceRef.Get(), Class);
	if (!Result) OutError = TEXT("CreateWidget failed for WBP_GCS_RouteWorkflow.");
	return Result;
}

bool UGCSRouteWorkflowCoordinator::ResolveRequiredWidgets(UUserWidget* Owner, const TArray<TPair<FName, UClass*>>& Requirements, TMap<FName, TWeakObjectPtr<UWidget>>& Out, FString& OutError) const
{
	Out.Reset();
	if (!Owner || !Owner->WidgetTree) { OutError = TEXT("Required widget owner has no WidgetTree."); return false; }
	TArray<FString> Missing;
	for (const TPair<FName, UClass*>& Requirement : Requirements)
	{
		UWidget* Value = Owner->WidgetTree->FindWidget(Requirement.Key);
		if (Value && Value->IsA(Requirement.Value)) Out.Add(Requirement.Key, Value); else Missing.Add(Requirement.Key.ToString());
	}
	if (Missing.IsEmpty()) return true;
	Out.Reset();
	OutError = FString::Printf(TEXT("%s missing/wrong required widgets: %s"), *Owner->GetName(), *FString::Join(Missing, TEXT(", ")));
	UE_LOG(LogGCSRouteWorkflow, Error, TEXT("%s"), *OutError);
	return false;
}

UUserWidget* UGCSRouteWorkflowCoordinator::ResolveNestedPage(UUserWidget* Workflow, const FName Preferred, const FName UniqueChild) const
{
	if (!Workflow || !Workflow->WidgetTree) return nullptr;
	if (UUserWidget* Direct = Cast<UUserWidget>(Workflow->WidgetTree->FindWidget(Preferred)))
		if (Direct->WidgetTree && Direct->WidgetTree->FindWidget(UniqueChild)) return Direct;
	TArray<UWidget*> Widgets; Workflow->WidgetTree->GetAllWidgets(Widgets);
	for (UWidget* Value : Widgets)
		if (UUserWidget* Candidate = Cast<UUserWidget>(Value))
			if (Candidate->WidgetTree && Candidate->WidgetTree->FindWidget(UniqueChild)) return Candidate;
	return nullptr;
}

bool UGCSRouteWorkflowCoordinator::BindWorkflowWidgets(UUserWidget* Workflow, FString& OutError)
{
	using namespace GCSRouteWorkflowPrivate;
	TMap<FName, TWeakObjectPtr<UWidget>> WorkflowMap;
	if (!ResolveRequiredWidgets(Workflow, Required({TEXT("BTN_RouteConvertTab"),TEXT("BTN_RouteUploadTab"),TEXT("BTN_RouteClose"),TEXT("SWITCHER_RoutePages")}), WorkflowMap, OutError)) return false;
	UUserWidget* Convert = ResolveNestedPage(Workflow, TEXT("WBP_GCS_RouteConvert"), TEXT("TXT_RouteCount"));
	UUserWidget* Upload = ResolveNestedPage(Workflow, TEXT("WBP_GCS_RouteUpload"), TEXT("TXT_UploadJobStateValue"));
	if (!Convert || !Upload) { OutError = TEXT("RouteConvert or RouteUpload nested page missing."); return false; }
	if (!ResolveRequiredWidgets(Convert, Required({
		TEXT("TXT_RouteFilePath"),TEXT("TXT_RouteCount"),TEXT("TXT_FrameCount"),TEXT("TXT_DatasetState"),
		TEXT("BTN_RouteBrowse"),TEXT("BTN_OutputBrowse"),TEXT("BTN_LoadPreview"),TEXT("BTN_GenerateWaypoints"),
		TEXT("EDT_OriginLatitude"),TEXT("EDT_OriginLongitude"),TEXT("EDT_OriginAltitude"),
		TEXT("TXT_OutputPath"),TEXT("TXT_StateDatasetValue"),TEXT("TXT_StatePreviewValue"),TEXT("TXT_StateArtifactValue"),
		TEXT("PROGRESS_Convert"),TEXT("TXT_GeneratedCount"),TEXT("TXT_ValidatedCount"),TEXT("TXT_ConvertPercent"),
		TEXT("TXT_ArtifactReadyValue"),TEXT("TXT_ArtifactIdValue"),TEXT("TXT_ArtifactElapsedValue")}), ConvertWidgets, OutError)) return false;
	if (!ResolveRequiredWidgets(Upload, Required({
		TEXT("TXT_ArtifactReadyValue"),TEXT("TXT_ConsoleValidatedValue"),TEXT("TXT_UploadMatched"),TEXT("TXT_UploadVacant"),TEXT("TXT_UploadSpare"),
		TEXT("TXT_UploadArtifactIdValue"),TEXT("TXT_UploadWaypointsRootValue"),TEXT("TXT_UploadSourceDatasetValue"),TEXT("LIST_RouteUploadStates"),
		TEXT("TXT_UploadWaiting"),TEXT("TXT_UploadPreparing"),TEXT("TXT_UploadUploading"),TEXT("TXT_UploadRetryWaiting"),
		TEXT("TXT_UploadSucceeded"),TEXT("TXT_UploadFailed"),TEXT("TXT_UploadCancelled"),TEXT("TXT_UploadJobStateValue"),
		TEXT("TXT_UploadThroughputValue"),TEXT("TXT_UploadBytesValue"),TEXT("TXT_UploadElapsedValue"),TEXT("PROGRESS_Upload"),
		TEXT("TXT_UploadPercent"),TEXT("EDT_MaxConcurrency"),TEXT("BTN_StartUpload"),TEXT("BTN_PauseUpload"),
		TEXT("TXT_CurrentMaxConcurrencyValue"),TEXT("BTN_ResumeUpload"),TEXT("BTN_CancelUpload"),
		TEXT("BTN_RetryFailed"),TEXT("BTN_ApplyMaxConcurrency")}), UploadWidgets, OutError)) return false;
	WorkflowWidgets = MoveTemp(WorkflowMap);
	RouteConvertWidgetRef = Convert; RouteUploadWidgetRef = Upload;
	ClearRouteListModel();
	SetEdit(UploadWidgets, TEXT("EDT_MaxConcurrency"), TEXT("1"));
	ApplyReadOnlyControlState();
	BindCommandControls();
	bBindingValid = true; return true;
}

void UGCSRouteWorkflowCoordinator::ApplyReadOnlyControlState()
{
	using namespace GCSRouteWorkflowPrivate;
	for (const FName Name : {FName(TEXT("BTN_StartUpload")),FName(TEXT("BTN_PauseUpload")),
		FName(TEXT("BTN_ResumeUpload")),FName(TEXT("BTN_CancelUpload")),
		FName(TEXT("BTN_RetryFailed")),FName(TEXT("BTN_ApplyMaxConcurrency"))})
		if (UButton* Button = Get<UButton>(UploadWidgets, Name)) Button->SetIsEnabled(false);
	if (UEditableTextBox* Box = Get<UEditableTextBox>(UploadWidgets, TEXT("EDT_MaxConcurrency")))
	{
		Box->SetIsReadOnly(false);
		Box->SetIsEnabled(true);
	}
	for (const FName Name : {FName(TEXT("EDT_OriginLatitude")),FName(TEXT("EDT_OriginLongitude")),FName(TEXT("EDT_OriginAltitude"))})
		if (UEditableTextBox* Box = Get<UEditableTextBox>(ConvertWidgets, Name)) { Box->SetIsReadOnly(false); Box->SetIsEnabled(true); }
}

void UGCSRouteWorkflowCoordinator::BindCommandControls()
{
	using namespace GCSRouteWorkflowPrivate;
	if (UButton* Button = Get<UButton>(WorkflowWidgets, TEXT("BTN_RouteClose")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteCloseClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteCloseClicked);
	}
	if (UButton* Button = Get<UButton>(WorkflowWidgets, TEXT("BTN_RouteConvertTab")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteConvertTabClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteConvertTabClicked);
	}
	if (UButton* Button = Get<UButton>(WorkflowWidgets, TEXT("BTN_RouteUploadTab")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteUploadTabClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteUploadTabClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_RouteBrowse")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteBrowseClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteBrowseClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_OutputBrowse")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleOutputBrowseClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleOutputBrowseClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_LoadPreview")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleLoadPreviewClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleLoadPreviewClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_GenerateWaypoints")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleGenerateWaypointsClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleGenerateWaypointsClicked);
	}
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_StartUpload")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleStartUploadClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleStartUploadClicked);
	}
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_PauseUpload")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandlePauseUploadClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandlePauseUploadClicked);
	}
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_ResumeUpload")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleResumeUploadClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleResumeUploadClicked);
	}
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_CancelUpload")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCancelUploadClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCancelUploadClicked);
	}
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_RetryFailed")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRetryFailedClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRetryFailedClicked);
	}
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_ApplyMaxConcurrency")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyMaxConcurrencyClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyMaxConcurrencyClicked);
	}
}

void UGCSRouteWorkflowCoordinator::UnbindCommandControls()
{
	using namespace GCSRouteWorkflowPrivate;
	if (UButton* Button = Get<UButton>(WorkflowWidgets, TEXT("BTN_RouteClose")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteCloseClicked);
	if (UButton* Button = Get<UButton>(WorkflowWidgets, TEXT("BTN_RouteConvertTab")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteConvertTabClicked);
	if (UButton* Button = Get<UButton>(WorkflowWidgets, TEXT("BTN_RouteUploadTab")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteUploadTabClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_RouteBrowse")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteBrowseClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_OutputBrowse")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleOutputBrowseClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_LoadPreview")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleLoadPreviewClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_GenerateWaypoints")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleGenerateWaypointsClicked);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_StartUpload")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleStartUploadClicked);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_PauseUpload")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandlePauseUploadClicked);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_ResumeUpload")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleResumeUploadClicked);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_CancelUpload")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCancelUploadClicked);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_RetryFailed")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRetryFailedClicked);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_ApplyMaxConcurrency")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyMaxConcurrencyClicked);
}

void UGCSRouteWorkflowCoordinator::HandleRouteBrowseClicked()
{
	if (!IsInGameThread() || bCommandInFlight || !GameInstanceRef.IsValid()) return;
	const FGCSRouteWorkflowCommandGate Gate = ComputeCommandGate(GatherCommandGateInputs());
	if (!Gate.bRouteBrowseEnabled) return;

	enum class EResult : uint8 { Cancelled, Succeeded, Failed };
	EResult Result = EResult::Cancelled;
	FString SelectedPath;
	FString Error;
	{
		TGuardValue<bool> Guard(bCommandInFlight, true);
#if WITH_DEV_AUTOMATION_TESTS
		const bool bSelected = CommandTestHooks.OpenNpzDialog
			? CommandTestHooks.OpenNpzDialog(SelectedPath)
			: OpenRuntimeNpzFileDialog(SelectedPath);
#else
		const bool bSelected = OpenRuntimeNpzFileDialog(SelectedPath);
#endif
		if (bSelected)
		{
			if (!FPaths::GetExtension(SelectedPath).Equals(TEXT("npz"), ESearchCase::IgnoreCase))
			{
				Error = TEXT("Selected file must be a .npz archive.");
				Result = EResult::Failed;
			}
			else
			{
				URoutePreviewSubsystem* Preview =
					GameInstanceRef->GetSubsystem<URoutePreviewSubsystem>();
#if WITH_DEV_AUTOMATION_TESTS
				const bool bPrepared = CommandTestHooks.PrepareRoute
					? CommandTestHooks.PrepareRoute(SelectedPath, Error)
					: (Preview && Preview->PrepareRouteImport(SelectedPath, Error));
#else
				const bool bPrepared = Preview
					&& Preview->PrepareRouteImport(SelectedPath, Error);
#endif
				if (bPrepared)
				{
					SelectedRouteImportFilePath = FPaths::ConvertRelativePathToFull(SelectedPath);
					SelectedRouteSourceDigest = Preview && Preview->HasPreparedRoute()
						? Preview->GetPreparedSourceIdentity().SourceDatasetDigest.ToLowerHex()
						: FString();
					Result = EResult::Succeeded;
				}
				else
				{
					if (Error.IsEmpty()) Error = TEXT("Route Preview authority rejected the NPZ import.");
					Result = EResult::Failed;
				}
			}
		}
	}
	if (Result == EResult::Succeeded)
	{
		ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Dataset);
		RefreshReadOnlyState();
	}
	else if (Result == EResult::Failed)
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Dataset, Error);
	}
	UpdateCommandControlState();
}

void UGCSRouteWorkflowCoordinator::HandleOutputBrowseClicked()
{
	if (!IsInGameThread() || bCommandInFlight || !GameInstanceRef.IsValid()) return;
	const FGCSRouteWorkflowCommandGate Gate = ComputeCommandGate(GatherCommandGateInputs());
	if (!Gate.bOutputBrowseEnabled) return;

	enum class EResult : uint8 { Cancelled, Succeeded, Failed };
	EResult Result = EResult::Cancelled;
	FString SelectedRoot;
	FString ResolvedRoot;
	FString Error;
	{
		TGuardValue<bool> Guard(bCommandInFlight, true);
#if WITH_DEV_AUTOMATION_TESTS
		const bool bSelected = CommandTestHooks.OpenFolderDialog
			? CommandTestHooks.OpenFolderDialog(SelectedRoot)
			: OpenRuntimeFolderDialog(SelectedRoot);
#else
		const bool bSelected = OpenRuntimeFolderDialog(SelectedRoot);
#endif
		if (bSelected)
		{
			URouteArtifactWorkspaceSubsystem* Workspace =
				GameInstanceRef->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
#if WITH_DEV_AUTOMATION_TESTS
			const bool bSet = CommandTestHooks.SetWorkspaceRoot
				? CommandTestHooks.SetWorkspaceRoot(SelectedRoot, Error)
				: (Workspace && Workspace->SetConfiguredWorkspaceRoot(SelectedRoot, Error));
			const bool bResolved = bSet && (CommandTestHooks.ResolveWorkspaceRoot
				? CommandTestHooks.ResolveWorkspaceRoot(ResolvedRoot, Error)
				: (Workspace && Workspace->ResolveWorkspaceRoot(ResolvedRoot, Error)));
#else
			const bool bSet = Workspace
				&& Workspace->SetConfiguredWorkspaceRoot(SelectedRoot, Error);
			const bool bResolved = bSet && Workspace
				&& Workspace->ResolveWorkspaceRoot(ResolvedRoot, Error);
#endif
			if (bResolved)
			{
				Result = EResult::Succeeded;
			}
			else
			{
				if (Error.IsEmpty()) Error = TEXT("Artifact Workspace rejected the selected root.");
				Result = EResult::Failed;
			}
		}
	}
	if (Result == EResult::Succeeded)
	{
		ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Artifact);
		GCSRouteWorkflowPrivate::SetText(ConvertWidgets, TEXT("TXT_OutputPath"), ResolvedRoot);
		GCSRouteWorkflowPrivate::SetTooltip(ConvertWidgets, TEXT("TXT_OutputPath"), ResolvedRoot);
		RefreshReadOnlyState();
	}
	else if (Result == EResult::Failed)
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Artifact, Error);
	}
	UpdateCommandControlState();
}

void UGCSRouteWorkflowCoordinator::HandleLoadPreviewClicked()
{
	if (!IsInGameThread() || bCommandInFlight || !GameInstanceRef.IsValid()) return;
	if (IsAutoStartInFlight())
	{
		SetCommandError(
			EGCSRouteWorkflowCommandErrorCategory::Preview,
			TEXT("当前仍有上传准备事务运行，请等待完成。"));
		UpdateCommandControlState();
		return;
	}
	URoutePreviewSubsystem* Preview = GameInstanceRef->GetSubsystem<URoutePreviewSubsystem>();
	UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	const FGCSRouteWorkflowCommandGateInputs Inputs = GatherCommandGateInputs();
	if (Inputs.bGenerationActive || !Inputs.bHasPreparedRoute) return;

	FString Error;
#if WITH_DEV_AUTOMATION_TESTS
	const bool bMayReplaceUploadJob = CommandTestHooks.CanReplaceUploadJob
		? CommandTestHooks.CanReplaceUploadJob(Error)
		: (Upload && Upload->CanPrepareForNewUploadJob(Error));
#else
	const bool bMayReplaceUploadJob =
		Upload && Upload->CanPrepareForNewUploadJob(Error);
#endif
	if (!bMayReplaceUploadJob)
	{
		SetCommandError(
			EGCSRouteWorkflowCommandErrorCategory::Preview,
			TEXT("当前仍有上传任务运行，请先等待完成或显式取消。"));
		UpdateCommandControlState();
		return;
	}

	double Latitude = 0.0;
	double Longitude = 0.0;
	double Altitude = 0.0;
	using namespace GCSRouteWorkflowPrivate;
	const UEditableTextBox* LatitudeInput = Get<UEditableTextBox>(ConvertWidgets, TEXT("EDT_OriginLatitude"));
	const UEditableTextBox* LongitudeInput = Get<UEditableTextBox>(ConvertWidgets, TEXT("EDT_OriginLongitude"));
	const UEditableTextBox* AltitudeInput = Get<UEditableTextBox>(ConvertWidgets, TEXT("EDT_OriginAltitude"));
	if (!ParseOriginInputs(
		LatitudeInput ? LatitudeInput->GetText().ToString() : FString(),
		LongitudeInput ? LongitudeInput->GetText().ToString() : FString(),
		AltitudeInput ? AltitudeInput->GetText().ToString() : FString(),
		Latitude, Longitude, Altitude, Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview, Error);
		UpdateCommandControlState();
		return;
	}

	bool bSucceeded = false;
	{
		TGuardValue<bool> Guard(bCommandInFlight, true);
#if WITH_DEV_AUTOMATION_TESTS
		if (CommandTestHooks.CommitRouteWorkflow)
		{
			bSucceeded = CommandTestHooks.CommitRouteWorkflow(
				Latitude, Longitude, Altitude, true, Error);
		}
		else if (CommandTestHooks.CommitRoute || CommandTestHooks.SetOrigin ||
			CommandTestHooks.SetOverlay)
		{
			const bool bCommitted = CommandTestHooks.CommitRoute
				? CommandTestHooks.CommitRoute(Error)
				: false;
			const bool bOriginSet = bCommitted && CommandTestHooks.SetOrigin
				&& CommandTestHooks.SetOrigin(Latitude, Longitude, Altitude, Error);
			bSucceeded = bOriginSet && CommandTestHooks.SetOverlay
				&& CommandTestHooks.SetOverlay(true, Error);
		}
		else
		{
			bSucceeded = Preview && Preview->CommitPreparedRouteForWorkflow(
				Latitude, Longitude, Altitude, true, Error);
		}
#else
		bSucceeded = Preview && Preview->CommitPreparedRouteForWorkflow(
			Latitude, Longitude, Altitude, true, Error);
#endif
		if (bSucceeded)
		{
#if WITH_DEV_AUTOMATION_TESTS
			bSucceeded = CommandTestHooks.ClearUploadJobForNewDataset
				? CommandTestHooks.ClearUploadJobForNewDataset(Error)
				: (Upload && Upload->PrepareForNewUploadJob(Error));
#else
			bSucceeded = Upload && Upload->PrepareForNewUploadJob(Error);
#endif
		}
	}
	if (bSucceeded)
	{
		ResetAutoStartForNewDataset();
		ClearRouteListModel();
		ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview);
		RefreshReadOnlyState();
	}
	else
	{
		if (Error.IsEmpty()) Error = TEXT("Route Preview authority rejected the command.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview, Error);
	}
	UpdateCommandControlState();
}

void UGCSRouteWorkflowCoordinator::HandleGenerateWaypointsClicked()
{
	if (!IsInGameThread() || bCommandInFlight || !GameInstanceRef.IsValid()) return;
	const FGCSRouteWorkflowCommandGate Gate = ComputeCommandGate(GatherCommandGateInputs());
	if (!Gate.bGenerateWaypointsEnabled) return;

	URouteWaypointGenerationSubsystem* Generator =
		GameInstanceRef->GetSubsystem<URouteWaypointGenerationSubsystem>();
	FString Error;
	bool bStarted = false;
	{
		TGuardValue<bool> Guard(bCommandInFlight, true);
#if WITH_DEV_AUTOMATION_TESTS
		bStarted = CommandTestHooks.StartGeneration
			? CommandTestHooks.StartGeneration(0, Error)
			: (Generator && Generator->StartGeneration(0, Error));
#else
		bStarted = Generator && Generator->StartGeneration(0, Error);
#endif
	}
	if (bStarted)
	{
		ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Artifact);
		RefreshReadOnlyState();
	}
	else
	{
		if (Error.IsEmpty()) Error = TEXT("Waypoint Generation authority rejected the command.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Artifact, Error);
	}
	UpdateCommandControlState();
}

bool UGCSRouteWorkflowCoordinator::ReadConcurrencyDraft(
	int32& OutValue,
	FString& OutError) const
{
	const UEditableTextBox* Draft = GCSRouteWorkflowPrivate::Get<UEditableTextBox>(
		UploadWidgets, TEXT("EDT_MaxConcurrency"));
	return ParsePositiveConcurrency(
		Draft ? Draft->GetText().ToString() : FString(), OutValue, OutError);
}

void UGCSRouteWorkflowCoordinator::HandleStartUploadClicked()
{
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	if (!ComputeUploadCommandGate(GatherUploadCommandGateInputs()).bStartEnabled) return;
	FString Error;
	int32 RequestedMaximum = 1;
	if (GCSRouteWorkflowPrivate::Get<UEditableTextBox>(
		UploadWidgets, TEXT("EDT_MaxConcurrency")) &&
		(!ReadConcurrencyDraft(RequestedMaximum, Error) || RequestedMaximum != 1))
	{
		if (Error.IsEmpty())
		{
			Error = TEXT("E.0 safety freeze requires maximum concurrency to be exactly 1.");
		}
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
		return;
	}
	if (!BeginAutoStartTransaction(Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Automatic upload start was rejected.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
	}
	else
	{
		ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
	}
	RefreshReadOnlyState();
}

bool UGCSRouteWorkflowCoordinator::CaptureAutoStartSnapshot(
	FGCSRouteAutoStartSnapshot& OutSnapshot,
	FString& OutError) const
{
	OutSnapshot = FGCSRouteAutoStartSnapshot{};
	OutError.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	if (AutoStartTestHooks.CaptureSnapshot)
	{
		return AutoStartTestHooks.CaptureSnapshot(OutSnapshot, OutError);
	}
#endif
	if (!GameInstanceRef.IsValid())
	{
		OutError = TEXT("Route Workflow Coordinator is not initialized.");
		return false;
	}
	const UGameInstance* GI = GameInstanceRef.Get();
	const UGCSUIBridgeSubsystem* Bridge = GI->GetSubsystem<UGCSUIBridgeSubsystem>();
	const UFleetManagerSubsystem* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
	const URouteArtifactWorkspaceSubsystem* Workspace =
		GI->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	if (!Bridge || !Fleet || !Workspace)
	{
		OutError = TEXT("Selection, Fleet, or Artifact authority is unavailable.");
		return false;
	}

	OutSnapshot.SelectedDroneIDs = Bridge->GetSelectedCommandDroneIDs();
	OutSnapshot.SelectedDroneIDs.Sort();
	if (OutSnapshot.SelectedDroneIDs.IsEmpty())
	{
		OutError = TEXT("No drones selected.");
		return false;
	}
	OutSnapshot.FleetSessionID = Fleet->GetActiveFleetSessionID();
	OutSnapshot.FleetRevision = Fleet->GetFleetRevision();
	if (OutSnapshot.FleetSessionID.TrimStartAndEnd().IsEmpty() ||
		OutSnapshot.FleetRevision < 0)
	{
		OutError = TEXT("Current Fleet identity is invalid.");
		return false;
	}

	const ERouteArtifactRecoveryStatus Recovery = Workspace->GetLastRecoveryResult().Status;
	if (Workspace->GetCurrentStatus() != ERouteArtifactCurrentStatus::Ready ||
		(Recovery != ERouteArtifactRecoveryStatus::Stable &&
		 Recovery != ERouteArtifactRecoveryStatus::Recovered))
	{
		OutError = TEXT("Artifact is not READY.");
		return false;
	}
	const FRouteArtifactCurrentInfo& Info = Workspace->GetCurrentInfo();
	if (Info.ArtifactSetID.IsEmpty() || !Info.ArtifactSetDigest.IsValid() ||
		!Info.SourceDatasetDigest.IsValid() || Info.RouteSlotCount <= 0)
	{
		OutError = TEXT("Current Artifact identity is invalid.");
		return false;
	}
	OutSnapshot.ArtifactSetID = Info.ArtifactSetID;
	OutSnapshot.ArtifactSetDigest = Info.ArtifactSetDigest.ToLowerHex();
	OutSnapshot.SourceDatasetDigest = Info.SourceDatasetDigest.ToLowerHex();
	OutSnapshot.RouteSlotCount = Info.RouteSlotCount;

	TSet<int32> NumericIDs;
	TSet<int32> RouteSlots;
	OutSnapshot.Targets.Reserve(OutSnapshot.SelectedDroneIDs.Num());
	for (const FString& DroneID : OutSnapshot.SelectedDroneIDs)
	{
		FTCHARToUTF8 EncodedDroneID(*DroneID);
		std::uint64_t NumericID64 = 0;
		if (!dronegcs::identity::TryParseDroneLogicalId(
			std::string_view(EncodedDroneID.Get(), EncodedDroneID.Length()), NumericID64) ||
			NumericID64 > static_cast<std::uint64_t>(MAX_int32))
		{
			OutError = FString::Printf(TEXT("Invalid canonical DroneID: %s"), *DroneID);
			return false;
		}
		const int32 NumericID = static_cast<int32>(NumericID64);
		const int32 RouteSlot = NumericID - 1;
		if (NumericIDs.Contains(NumericID) || RouteSlots.Contains(RouteSlot))
		{
			OutError = FString::Printf(TEXT("Duplicate numeric DroneID or RouteSlot: %s"), *DroneID);
			return false;
		}
		if (RouteSlot < 0 || RouteSlot >= OutSnapshot.RouteSlotCount)
		{
			OutError = FString::Printf(
				TEXT("RouteSlot %d for %s is outside Manifest RouteSlotCount %d."),
				RouteSlot, *DroneID, OutSnapshot.RouteSlotCount);
			return false;
		}

		FDroneData Drone;
		if (!Fleet->GetDroneByID(DroneID, Drone))
		{
			OutError = FString::Printf(TEXT("Selected drone does not exist: %s"), *DroneID);
			return false;
		}
		if (!Drone.bOnline)
		{
			OutError = FString::Printf(TEXT("Selected drone is offline: %s"), *DroneID);
			return false;
		}
		if (Drone.EndpointIdentity.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("Selected drone endpoint identity is invalid: %s"), *DroneID);
			return false;
		}

		FRouteArtifactRecord Record;
		if (!Workspace->FindCurrentRouteRecord(RouteSlot, Record) ||
			Record.RouteSlotIndex != RouteSlot || Record.RelativePath.IsEmpty() ||
			Record.FileByteSize <= 0 || !Record.RouteArtifactDigest.IsValid())
		{
			OutError = FString::Printf(
				TEXT("Manifest Route Record is missing or invalid for RouteSlot %d."), RouteSlot);
			return false;
		}

		NumericIDs.Add(NumericID);
		RouteSlots.Add(RouteSlot);
		FGCSRouteAutoStartTargetSnapshot Target;
		Target.DroneID = DroneID;
		Target.EndpointIdentity = Drone.EndpointIdentity;
		Target.NumericDroneID = NumericID;
		Target.RouteSlotIndex = RouteSlot;
		Target.RouteRelativePath = Record.RelativePath;
		Target.RouteFileByteSize = Record.FileByteSize;
		Target.RouteDigest = Record.RouteArtifactDigest.ToLowerHex();
		OutSnapshot.Targets.Add(MoveTemp(Target));
	}
	return true;
}

bool UGCSRouteWorkflowCoordinator::BeginAutoStartTransaction(FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (IsAutoStartInFlight())
	{
		OutError = TEXT("An automatic assignment transaction is already in progress.");
		return false;
	}

	FGCSRouteAutoStartSnapshot Candidate;
	if (!CaptureAutoStartSnapshot(Candidate, OutError)) return false;
#if WITH_DEV_AUTOMATION_TESTS
	const bool bUsingAutoStartTestHooks = static_cast<bool>(AutoStartTestHooks.CaptureSnapshot);
#else
	const bool bUsingAutoStartTestHooks = false;
#endif
	if (!bUsingAutoStartTestHooks)
	{
		UDSPGCSUploadJobSubsystem* Upload = GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
			GameInstanceRef->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
		if (!Upload || !Sync)
		{
			OutError = TEXT("Upload Job or Frozen Assignment Sync subsystem is unavailable.");
			return false;
		}
		const FDSPGCSUploadJobCommandStatus Command = Upload->GetCommandStatus();
		if (Command.bHasActiveJobBinding || Command.bHasExecutableJob || Command.bCommandInFlight)
		{
			OutError = TEXT("An Upload Job is already active or starting.");
			return false;
		}
		if (Sync->GetSyncState() == EDSPGCSFrozenAssignmentSyncState::SyncingToConsole)
		{
			OutError = TEXT("A Frozen Assignment sync is already active.");
			return false;
		}
	}

	Candidate.TransactionID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	AutoStartSnapshot = MoveTemp(Candidate);
	AutoStartState = EGCSRouteAutoStartState::ValidatingMapping;
	AutoStartDiagnostic.Reset();
	ReplaceAutoStartPreviewItems(AutoStartSnapshot);
	for (const FGCSRouteAutoStartTargetSnapshot& Target : AutoStartSnapshot.Targets)
	{
		UE_LOG(LogGCSRouteWorkflow, Display,
			TEXT("[ROUTE_AUTO_ASSIGN] Transaction=%s DroneID=%s Endpoint=%s RouteSlot=%d Route=%s Bytes=%lld Digest=%s"),
			*AutoStartSnapshot.TransactionID, *Target.DroneID, *Target.EndpointIdentity,
			Target.RouteSlotIndex, *Target.RouteRelativePath, Target.RouteFileByteSize,
			*Target.RouteDigest);
	}

#if WITH_DEV_AUTOMATION_TESTS
	if (AutoStartTestHooks.BuildAssignment)
	{
		if (!AutoStartTestHooks.BuildAssignment(AutoStartSnapshot, OutError))
		{
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
	}
	else
#endif
	{
		URouteAssignmentSubsystem* Assignment =
			GameInstanceRef->GetSubsystem<URouteAssignmentSubsystem>();
		if (!Assignment)
		{
			OutError = TEXT("Route Assignment authority is unavailable.");
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
		FRouteAssignmentSourceSnapshot Source;
		if (!Assignment->CaptureSourceSnapshot(Source, OutError) ||
			Source.SourceIdentity.FleetSessionID != AutoStartSnapshot.FleetSessionID ||
			Source.SourceIdentity.FleetRevisionAtCapture != AutoStartSnapshot.FleetRevision ||
			Source.RouteSlots.Num() != AutoStartSnapshot.RouteSlotCount ||
			Source.SourceIdentity.SourceDatasetDigest.ToLowerHex() !=
				AutoStartSnapshot.SourceDatasetDigest)
		{
			if (OutError.IsEmpty()) OutError = TEXT("Assignment source changed during mapping validation.");
			FinishAutoStart(EGCSRouteAutoStartState::CancelledAsStale, OutError);
			return false;
		}
		AutoStartSnapshot.AssignmentSourceIdentity = Source.SourceIdentity;
		FRouteAssignmentExplicitBindingRequest Request;
		Request.ExpectedSourceIdentity = Source.SourceIdentity;
		for (const FGCSRouteAutoStartTargetSnapshot& Target : AutoStartSnapshot.Targets)
		{
			FRouteAssignmentExplicitBinding Binding;
			Binding.DroneID = Target.DroneID;
			Binding.RouteSlotIndex = Target.RouteSlotIndex;
			Request.Bindings.Add(MoveTemp(Binding));
		}
		Assignment->Reset();
		if (!Assignment->BuildAndReplaceExplicitWorkingAssignment(Request, OutError))
		{
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
	}

	AutoStartState = EGCSRouteAutoStartState::Freezing;
#if WITH_DEV_AUTOMATION_TESTS
	if (AutoStartTestHooks.FreezeAssignment)
	{
		if (!AutoStartTestHooks.FreezeAssignment(AutoStartSnapshot, OutError))
		{
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
	}
	else
#endif
	{
		URouteAssignmentSubsystem* Assignment =
			GameInstanceRef->GetSubsystem<URouteAssignmentSubsystem>();
		FRouteAssignmentSnapshot Frozen;
		if (!Assignment || !Assignment->Freeze(ERouteAssignmentFreezePolicy::AllowDegraded, OutError) ||
			!Assignment->GetFrozenSnapshot(Frozen))
		{
			if (OutError.IsEmpty()) OutError = TEXT("Freeze did not produce an immutable snapshot.");
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
		AutoStartSnapshot.AssignmentAuthoritySessionID = Frozen.AssignmentAuthoritySessionID;
		AutoStartSnapshot.AssignmentRevision = Frozen.AssignmentRevision;
	}

#if WITH_DEV_AUTOMATION_TESTS
	if (AutoStartTestHooks.StartSync)
	{
		if (!AutoStartTestHooks.StartSync(AutoStartSnapshot, OutError))
		{
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
	}
	else
#endif
	{
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
			GameInstanceRef->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
		if (!Sync || !Sync->StartSyncFrozenAssignment(OutError))
		{
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
		const FDSPGCSFrozenAssignmentSyncStatus Status = Sync->GetSyncStatus();
		AutoStartSnapshot.SyncTransactionID = Status.TransactionID;
		AutoStartSnapshot.AssignmentDigest = Status.CanonicalPayloadDigest;
		if (Status.FleetSessionID != AutoStartSnapshot.FleetSessionID ||
			Status.AssignmentAuthoritySessionID != AutoStartSnapshot.AssignmentAuthoritySessionID ||
			Status.AssignmentRevision != AutoStartSnapshot.AssignmentRevision ||
			Status.TransactionID.IsEmpty() || Status.CanonicalPayloadDigest.IsEmpty())
		{
			OutError = TEXT("Frozen Assignment sync did not bind the current automatic transaction.");
			FinishAutoStart(EGCSRouteAutoStartState::Failed, OutError);
			return false;
		}
	}

	AutoStartState = EGCSRouteAutoStartState::SyncingConsole;
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[ROUTE_AUTO_START] Transaction=%s State=SyncingConsole Targets=%d MaxConcurrentTargets=1"),
		*AutoStartSnapshot.TransactionID, AutoStartSnapshot.Targets.Num());
	AdvanceAutoStartTransaction();
	return AutoStartState != EGCSRouteAutoStartState::Failed &&
		AutoStartState != EGCSRouteAutoStartState::CancelledAsStale;
}

FGCSRouteAutoStartSyncObservation UGCSRouteWorkflowCoordinator::ObserveAutoStartSync() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AutoStartTestHooks.ObserveSync) return AutoStartTestHooks.ObserveSync();
#endif
	FGCSRouteAutoStartSyncObservation Result;
	if (!GameInstanceRef.IsValid()) return Result;
	const UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
		GameInstanceRef->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
	if (!Sync) return Result;
	const FDSPGCSFrozenAssignmentSyncStatus Status = Sync->GetSyncStatus();
	Result.State = static_cast<uint8>(Status.State);
	Result.FleetSessionID = Status.FleetSessionID;
	Result.AssignmentAuthoritySessionID = Status.AssignmentAuthoritySessionID;
	Result.AssignmentRevision = Status.AssignmentRevision;
	Result.AssignmentDigest = Status.CanonicalPayloadDigest;
	Result.TransactionID = Status.TransactionID;
	Result.LastError = Status.LastError;
	return Result;
}

bool UGCSRouteWorkflowCoordinator::ValidateAutoStartSnapshotStillCurrent(FString& OutError) const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AutoStartTestHooks.ValidateSnapshot)
		return AutoStartTestHooks.ValidateSnapshot(AutoStartSnapshot, OutError);
#endif
	FGCSRouteAutoStartSnapshot Current;
	if (!CaptureAutoStartSnapshot(Current, OutError)) return false;
	if (Current.SelectedDroneIDs != AutoStartSnapshot.SelectedDroneIDs ||
		Current.FleetSessionID != AutoStartSnapshot.FleetSessionID ||
		Current.FleetRevision != AutoStartSnapshot.FleetRevision ||
		Current.ArtifactSetID != AutoStartSnapshot.ArtifactSetID ||
		Current.ArtifactSetDigest != AutoStartSnapshot.ArtifactSetDigest ||
		Current.SourceDatasetDigest != AutoStartSnapshot.SourceDatasetDigest ||
		Current.RouteSlotCount != AutoStartSnapshot.RouteSlotCount ||
		Current.Targets.Num() != AutoStartSnapshot.Targets.Num())
	{
		OutError = TEXT("Selection, Fleet, or Artifact identity changed during Console sync.");
		return false;
	}
	for (int32 Index = 0; Index < Current.Targets.Num(); ++Index)
	{
		if (!GCSRouteWorkflowPrivate::SameAutoStartTarget(
			Current.Targets[Index], AutoStartSnapshot.Targets[Index]))
		{
			OutError = FString::Printf(TEXT("Target identity changed during Console sync: %s"),
				*AutoStartSnapshot.Targets[Index].DroneID);
			return false;
		}
	}

	const URouteAssignmentSubsystem* Assignment =
		GameInstanceRef->GetSubsystem<URouteAssignmentSubsystem>();
	FRouteAssignmentSourceSnapshot Source;
	FRouteAssignmentSnapshot Frozen;
	if (!Assignment || !Assignment->CaptureSourceSnapshot(Source, OutError) ||
		!GCSRouteWorkflowPrivate::SameAssignmentSource(
			Source.SourceIdentity, AutoStartSnapshot.AssignmentSourceIdentity) ||
		!Assignment->GetFrozenSnapshot(Frozen) ||
		Frozen.AssignmentAuthoritySessionID != AutoStartSnapshot.AssignmentAuthoritySessionID ||
		Frozen.AssignmentRevision != AutoStartSnapshot.AssignmentRevision)
	{
		if (OutError.IsEmpty()) OutError = TEXT("Frozen Assignment authority changed during Console sync.");
		return false;
	}
	const UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	if (!Upload)
	{
		OutError = TEXT("Upload Job subsystem is unavailable.");
		return false;
	}
	const FDSPGCSUploadJobCommandStatus Command = Upload->GetCommandStatus();
	if (Command.bHasActiveJobBinding || Command.bHasExecutableJob || Command.bCommandInFlight)
	{
		OutError = TEXT("Another Upload Job became active during Console sync.");
		return false;
	}
	return true;
}

void UGCSRouteWorkflowCoordinator::AdvanceAutoStartTransaction()
{
	if (!IsInGameThread() || AutoStartState != EGCSRouteAutoStartState::SyncingConsole) return;
	const FGCSRouteAutoStartSyncObservation Sync = ObserveAutoStartSync();
	const EDSPGCSFrozenAssignmentSyncState SyncState =
		static_cast<EDSPGCSFrozenAssignmentSyncState>(Sync.State);
	if (SyncState == EDSPGCSFrozenAssignmentSyncState::SyncFailed)
	{
		FinishAutoStart(EGCSRouteAutoStartState::Failed,
			Sync.LastError.IsEmpty() ? TEXT("Console rejected or timed out Frozen Assignment sync.") : Sync.LastError);
		return;
	}
	if (SyncState != EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted)
	{
		if (SyncState != EDSPGCSFrozenAssignmentSyncState::SyncingToConsole)
		{
			FinishAutoStart(EGCSRouteAutoStartState::Failed,
				TEXT("Frozen Assignment sync left the active transaction unexpectedly."));
		}
		return;
	}
	if (Sync.TransactionID != AutoStartSnapshot.SyncTransactionID ||
		Sync.FleetSessionID != AutoStartSnapshot.FleetSessionID ||
		Sync.AssignmentAuthoritySessionID != AutoStartSnapshot.AssignmentAuthoritySessionID ||
		Sync.AssignmentRevision != AutoStartSnapshot.AssignmentRevision ||
		Sync.AssignmentDigest != AutoStartSnapshot.AssignmentDigest)
	{
		FinishAutoStart(EGCSRouteAutoStartState::CancelledAsStale,
			TEXT("Ignored stale or cross-transaction Frozen Assignment Accepted ACK."));
		return;
	}

	FString Error;
	if (!ValidateAutoStartSnapshotStillCurrent(Error))
	{
		FinishAutoStart(EGCSRouteAutoStartState::CancelledAsStale, Error);
		return;
	}
	AutoStartState = EGCSRouteAutoStartState::StartingJob;
	if (AutoStartSnapshot.bJobStartAttempted) return;
	AutoStartSnapshot.bJobStartAttempted = true;
	FString UploadJobID;
	bool bStarted = false;
#if WITH_DEV_AUTOMATION_TESTS
	if (AutoStartTestHooks.StartJob)
	{
		bStarted = AutoStartTestHooks.StartJob(1, UploadJobID, Error);
	}
	else
#endif
	{
		UDSPGCSUploadJobSubsystem* Upload =
			GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
		bStarted = Upload && Upload->StartUploadJobFromCurrentAuthorities(1, UploadJobID, Error);
	}
	if (!bStarted)
	{
		if (Error.IsEmpty()) Error = TEXT("Upload Job Start was rejected.");
		FinishAutoStart(EGCSRouteAutoStartState::Failed, Error);
		return;
	}
	AutoStartSnapshot.bJobStartDispatched = true;
	FinishAutoStart(EGCSRouteAutoStartState::Completed, FString());
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[ROUTE_AUTO_START] Transaction=%s State=Completed UploadJobID=%s Targets=%d MaxConcurrentTargets=1"),
		*AutoStartSnapshot.TransactionID, *UploadJobID, AutoStartSnapshot.Targets.Num());
}

void UGCSRouteWorkflowCoordinator::FinishAutoStart(
	const EGCSRouteAutoStartState FinalState,
	const FString& Diagnostic)
{
	AutoStartState = FinalState;
	AutoStartDiagnostic = Diagnostic.Left(320);
	if (!AutoStartDiagnostic.IsEmpty())
	{
		UE_LOG(LogGCSRouteWorkflow, Warning,
			TEXT("[ROUTE_AUTO_START] Transaction=%s State=%s Diagnostic=%s"),
			*AutoStartSnapshot.TransactionID, *FormatAutoStartState(FinalState),
			*AutoStartDiagnostic);
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, AutoStartDiagnostic);
	}
}

void UGCSRouteWorkflowCoordinator::ResetAutoStartForNewDataset()
{
	if (IsAutoStartInFlight()) return;
	AutoStartState = EGCSRouteAutoStartState::Idle;
	AutoStartSnapshot = FGCSRouteAutoStartSnapshot{};
	AutoStartDiagnostic.Reset();
}

bool UGCSRouteWorkflowCoordinator::IsAutoStartInFlight() const
{
	switch (AutoStartState)
	{
	case EGCSRouteAutoStartState::ValidatingMapping:
	case EGCSRouteAutoStartState::Freezing:
	case EGCSRouteAutoStartState::SyncingConsole:
	case EGCSRouteAutoStartState::StartingJob:
		return true;
	default:
		return false;
	}
}

bool UGCSRouteWorkflowCoordinator::IsCommandSelectionMutationBlocked() const
{
	if (bCommandInFlight || IsAutoStartInFlight())
	{
		return true;
	}
	if (!GameInstanceRef.IsValid())
	{
		return false;
	}

	const UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	if (!Upload)
	{
		return false;
	}
	const FDSPGCSUploadJobCommandStatus Command = Upload->GetCommandStatus();
	if (Command.bCommandInFlight)
	{
		return true;
	}
	if (const dspgcs::UploadJobSnapshotDTO* Snapshot =
		Upload->GetStateMirror().GetSnapshot())
	{
		const bool bTerminal = Snapshot->jobState == dspgcs::UploadJobState::Completed
			|| Snapshot->jobState == dspgcs::UploadJobState::Failed
			|| Snapshot->jobState == dspgcs::UploadJobState::Cancelled;
		if (bTerminal)
		{
			return false;
		}
	}
	return Command.bHasActiveJobBinding || Command.bHasExecutableJob;
}

void UGCSRouteWorkflowCoordinator::HandlePauseUploadClicked()
{
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	if (!ComputeUploadCommandGate(GatherUploadCommandGateInputs()).bPauseEnabled) return;
	UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	FString Error;
	if (!Upload || !Upload->PauseUploadJob(Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Pause command was rejected.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
	}
	else ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
	RefreshReadOnlyState();
}

void UGCSRouteWorkflowCoordinator::HandleResumeUploadClicked()
{
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	if (!ComputeUploadCommandGate(GatherUploadCommandGateInputs()).bResumeEnabled) return;
	UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	FString Error;
	if (!Upload || !Upload->ResumeUploadJob(Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Resume command was rejected.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
	}
	else ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
	RefreshReadOnlyState();
}

void UGCSRouteWorkflowCoordinator::HandleCancelUploadClicked()
{
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	if (!ComputeUploadCommandGate(GatherUploadCommandGateInputs()).bCancelEnabled) return;
	UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	FString Error;
	if (!Upload || !Upload->CancelUploadJob(Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Cancel command was rejected.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
	}
	else ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
	RefreshReadOnlyState();
}

void UGCSRouteWorkflowCoordinator::HandleRetryFailedClicked()
{
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	if (!ComputeUploadCommandGate(
		GatherUploadCommandGateInputs()).bRetryFailedEnabled) return;
	UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	FString Error;
	if (!Upload || !Upload->RetryFailedRoutes(Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Retry Failed command was rejected.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
	}
	else ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
	RefreshReadOnlyState();
}

void UGCSRouteWorkflowCoordinator::HandleApplyMaxConcurrencyClicked()
{
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	if (!ComputeUploadCommandGate(GatherUploadCommandGateInputs()).bApplyMaxEnabled) return;
	int32 Maximum = 0;
	FString Error;
	if (!ReadConcurrencyDraft(Maximum, Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
		return;
	}
	if (Maximum != 1)
	{
		SetCommandError(
			EGCSRouteWorkflowCommandErrorCategory::Upload,
			TEXT("E.0 safety freeze does not allow concurrency greater than 1."));
		return;
	}
	UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	if (!Upload || !Upload->SetMaxConcurrentUploads(Maximum, Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Maximum concurrency command was rejected.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
	}
	else ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
	RefreshReadOnlyState();
}

FGCSRouteWorkflowCommandGateInputs UGCSRouteWorkflowCoordinator::GatherCommandGateInputs() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (CommandGateInputsForTest.IsSet()) return CommandGateInputsForTest.GetValue();
#endif
	FGCSRouteWorkflowCommandGateInputs Inputs;
	Inputs.bCommandInFlight = bCommandInFlight || IsAutoStartInFlight();
	if (!GameInstanceRef.IsValid()) return Inputs;
	UGameInstance* GI = GameInstanceRef.Get();
	const URoutePreviewSubsystem* Preview = GI->GetSubsystem<URoutePreviewSubsystem>();
	const URouteArtifactWorkspaceSubsystem* Workspace =
		GI->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	const URouteWaypointGenerationSubsystem* Generator =
		GI->GetSubsystem<URouteWaypointGenerationSubsystem>();

	Inputs.bHasPreparedRoute = Preview && Preview->HasPreparedRoute();
	Inputs.bRouteLoaded = Preview && Preview->IsRouteLoaded();
	Inputs.bShowOriginSet = Inputs.bRouteLoaded && Preview->GetRouteData().bShowOriginSet;
	Inputs.bGenerationActive = Generator && Generator->IsGenerationActive();
	if (Workspace)
	{
		const ERouteArtifactRecoveryStatus RecoveryStatus =
			Workspace->GetLastRecoveryResult().Status;
		const ERouteArtifactGenerationStatus GenerationStatus =
			Workspace->GetGenerationStatus();
		Inputs.bWorkspaceReady = (GenerationStatus == ERouteArtifactGenerationStatus::None
				|| GenerationStatus == ERouteArtifactGenerationStatus::Failed)
			&& !Workspace->IsCurrentSwapInProgress()
			&& (RecoveryStatus == ERouteArtifactRecoveryStatus::Stable
				|| RecoveryStatus == ERouteArtifactRecoveryStatus::Recovered);
	}

	using namespace GCSRouteWorkflowPrivate;
	const UEditableTextBox* Latitude = Get<UEditableTextBox>(
		ConvertWidgets, TEXT("EDT_OriginLatitude"));
	const UEditableTextBox* Longitude = Get<UEditableTextBox>(
		ConvertWidgets, TEXT("EDT_OriginLongitude"));
	const UEditableTextBox* Altitude = Get<UEditableTextBox>(
		ConvertWidgets, TEXT("EDT_OriginAltitude"));
	double ParsedLatitude = 0.0;
	double ParsedLongitude = 0.0;
	double ParsedAltitude = 0.0;
	FString ParseError;
	Inputs.bOriginValid = ParseOriginInputs(
		Latitude ? Latitude->GetText().ToString() : FString(),
		Longitude ? Longitude->GetText().ToString() : FString(),
		Altitude ? Altitude->GetText().ToString() : FString(),
		ParsedLatitude, ParsedLongitude, ParsedAltitude, ParseError);
	return Inputs;
}

void UGCSRouteWorkflowCoordinator::UpdateCommandControlState()
{
	using namespace GCSRouteWorkflowPrivate;
	const FGCSRouteWorkflowCommandGateInputs Inputs = GatherCommandGateInputs();
	const FGCSRouteWorkflowCommandGate Gate = ComputeCommandGate(Inputs);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_RouteBrowse")))
		Button->SetIsEnabled(Gate.bRouteBrowseEnabled);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_OutputBrowse")))
		Button->SetIsEnabled(Gate.bOutputBrowseEnabled);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_LoadPreview")))
		Button->SetIsEnabled(Gate.bLoadPreviewEnabled);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_GenerateWaypoints")))
		Button->SetIsEnabled(Gate.bGenerateWaypointsEnabled);
	for (const FName Name : {FName(TEXT("EDT_OriginLatitude")),
		FName(TEXT("EDT_OriginLongitude")), FName(TEXT("EDT_OriginAltitude"))})
	{
		if (UEditableTextBox* Box = Get<UEditableTextBox>(ConvertWidgets, Name))
		{
			Box->SetIsReadOnly(Inputs.bGenerationActive || Inputs.bCommandInFlight);
			Box->SetIsEnabled(!Inputs.bGenerationActive && !Inputs.bCommandInFlight);
		}
	}
}

FGCSRouteUploadCommandGateInputs
UGCSRouteWorkflowCoordinator::GatherUploadCommandGateInputs() const
{
	FGCSRouteUploadCommandGateInputs Inputs;
	if (!GameInstanceRef.IsValid()) return Inputs;
	const UGameInstance* GI = GameInstanceRef.Get();
	const URouteArtifactWorkspaceSubsystem* Workspace =
		GI->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	const UDSPGCSFrozenAssignmentSyncSubsystem* Frozen =
		GI->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
	const UDSPGCSUploadJobSubsystem* Upload =
		GI->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	if (Workspace)
	{
		const ERouteArtifactRecoveryStatus Recovery =
			Workspace->GetLastRecoveryResult().Status;
		Inputs.bArtifactReady =
			Workspace->GetCurrentStatus() == ERouteArtifactCurrentStatus::Ready &&
			(Recovery == ERouteArtifactRecoveryStatus::Stable ||
			 Recovery == ERouteArtifactRecoveryStatus::Recovered);
	}
	Inputs.bFrozenConsoleAccepted = Frozen &&
		Frozen->GetSyncState() == EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted;
	Inputs.bAutoStartInFlight = IsAutoStartInFlight();
	if (!Inputs.bArtifactReady)
	{
		Inputs.StartBlockedReason = TEXT("Artifact is not READY.");
	}
	else if (Inputs.bAutoStartInFlight)
	{
		Inputs.StartBlockedReason = TEXT("Automatic assignment sync is in progress.");
	}
	else if (Frozen && Frozen->GetSyncState() ==
		EDSPGCSFrozenAssignmentSyncState::SyncingToConsole)
	{
		Inputs.StartBlockedReason = TEXT("Assignment sync is already in progress.");
	}
	else
	{
		FGCSRouteAutoStartSnapshot Candidate;
		FString MappingError;
		Inputs.bAutoStartMappingValid = CaptureAutoStartSnapshot(Candidate, MappingError);
		if (!Inputs.bAutoStartMappingValid)
		{
			Inputs.StartBlockedReason = MappingError;
		}
	}
	if (!Upload)
	{
		if (Inputs.StartBlockedReason.IsEmpty())
			Inputs.StartBlockedReason = TEXT("Upload Job subsystem is unavailable.");
		return Inputs;
	}
	const FDSPGCSUploadJobCommandStatus Command = Upload->GetCommandStatus();
	Inputs.bHasActiveJobBinding = Command.bHasActiveJobBinding;
	Inputs.bHasExecutableJob = Command.bHasExecutableJob;
	Inputs.bCommandInFlight = Command.bCommandInFlight || Inputs.bAutoStartInFlight;
	if (Command.bCommandInFlight)
		Inputs.StartBlockedReason = TEXT("An Upload Job command is in progress.");
	else if (Command.bHasActiveJobBinding || Command.bHasExecutableJob)
		Inputs.StartBlockedReason = TEXT("An Upload Job is already active.");
	if (const dspgcs::UploadJobSnapshotDTO* Snapshot =
		Upload->GetStateMirror().GetSnapshot())
	{
		Inputs.JobState = static_cast<uint8>(Snapshot->jobState);
		Inputs.FailedCount = Snapshot->failedCount;
	}
	return Inputs;
}

void UGCSRouteWorkflowCoordinator::UpdateUploadCommandControlState()
{
	using namespace GCSRouteWorkflowPrivate;
	const FGCSRouteUploadCommandGate Gate =
		ComputeUploadCommandGate(GatherUploadCommandGateInputs());
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_StartUpload")))
	{
		Button->SetIsEnabled(Gate.bStartEnabled);
		Button->SetToolTipText(FText::FromString(
			Gate.bStartEnabled ? TEXT("Automatic selected-drone assignment; E.0 concurrency = 1.")
				: Gate.StartBlockedReason));
	}
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_PauseUpload")))
		Button->SetIsEnabled(Gate.bPauseEnabled);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_ResumeUpload")))
		Button->SetIsEnabled(Gate.bResumeEnabled);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_CancelUpload")))
		Button->SetIsEnabled(Gate.bCancelEnabled);
	if (UButton* Button = Get<UButton>(UploadWidgets, TEXT("BTN_RetryFailed")))
		Button->SetIsEnabled(Gate.bRetryFailedEnabled);
	if (UButton* Button = Get<UButton>(
		UploadWidgets, TEXT("BTN_ApplyMaxConcurrency")))
		Button->SetIsEnabled(Gate.bApplyMaxEnabled);
}

void UGCSRouteWorkflowCoordinator::SetCommandError(
	const EGCSRouteWorkflowCommandErrorCategory Category,
	const FString& Message)
{
	CommandErrorCategory = Category;
	CommandErrorMessage = Message.IsEmpty()
		? TEXT("Command rejected.") : Message.Left(320);
	CommandErrorExpiresAtSeconds = FPlatformTime::Seconds() + CommandErrorLifetimeSeconds;
	UE_LOG(LogGCSRouteWorkflow, Warning, TEXT("Route Workflow command rejected: %s"),
		*CommandErrorMessage);
	ApplyCommandErrorPresentation();
}

void UGCSRouteWorkflowCoordinator::ClearCommandError(
	const EGCSRouteWorkflowCommandErrorCategory Category)
{
	if (CommandErrorCategory != Category) return;
	CommandErrorCategory = EGCSRouteWorkflowCommandErrorCategory::None;
	CommandErrorMessage.Reset();
	CommandErrorExpiresAtSeconds = 0.0;
}

void UGCSRouteWorkflowCoordinator::ApplyCommandErrorPresentation()
{
	if (CommandErrorCategory == EGCSRouteWorkflowCommandErrorCategory::None) return;
	if (FPlatformTime::Seconds() >= CommandErrorExpiresAtSeconds)
	{
		CommandErrorCategory = EGCSRouteWorkflowCommandErrorCategory::None;
		CommandErrorMessage.Reset();
		CommandErrorExpiresAtSeconds = 0.0;
		return;
	}
	using namespace GCSRouteWorkflowPrivate;
	switch (CommandErrorCategory)
	{
	case EGCSRouteWorkflowCommandErrorCategory::Dataset:
		SetText(ConvertWidgets, TEXT("TXT_StateDatasetValue"), CommandErrorMessage);
		break;
	case EGCSRouteWorkflowCommandErrorCategory::Preview:
		SetText(ConvertWidgets, TEXT("TXT_StatePreviewValue"), CommandErrorMessage);
		break;
	case EGCSRouteWorkflowCommandErrorCategory::Artifact:
		SetText(ConvertWidgets, TEXT("TXT_StateArtifactValue"), CommandErrorMessage);
		break;
	case EGCSRouteWorkflowCommandErrorCategory::Upload:
		SetText(UploadWidgets, TEXT("TXT_UploadJobStateValue"), CommandErrorMessage);
		break;
	default:
		break;
	}
}

void UGCSRouteWorkflowCoordinator::RefreshReadOnlyState()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !bBindingValid || !GameInstanceRef.IsValid()) return;
	UGameInstance* GI = GameInstanceRef.Get();
	if (URoutePreviewSubsystem* Preview = GI->GetSubsystem<URoutePreviewSubsystem>())
	{
		const bool Loaded = Preview->IsRouteLoaded(), Prepared = Preview->HasPreparedRoute();
		const FString Dataset = Prepared ? TEXT("Prepared") : (Loaded ? TEXT("Loaded") : TEXT("--"));
		SetText(ConvertWidgets,TEXT("TXT_DatasetState"),Dataset); SetText(ConvertWidgets,TEXT("TXT_StateDatasetValue"),Dataset);
		SetText(ConvertWidgets,TEXT("TXT_RouteCount"),FString::FromInt(Prepared ? Preview->GetPreparedDroneCount() : Preview->GetDroneCount())); SetText(ConvertWidgets,TEXT("TXT_FrameCount"),FString::FromInt(Prepared ? Preview->GetPreparedFrameCount() : Preview->GetFrameCount()));
		const FRouteArtifactSourceIdentity* ID = Prepared ? &Preview->GetPreparedSourceIdentity() : (Loaded ? &Preview->GetActiveSourceIdentity() : nullptr);
		const bool bSelectedPathMatches = ID && !SelectedRouteImportFilePath.IsEmpty()
			&& !SelectedRouteSourceDigest.IsEmpty()
			&& SelectedRouteSourceDigest == ID->SourceDatasetDigest.ToLowerHex();
		const FString RoutePathText = bSelectedPathMatches
			? SelectedRouteImportFilePath
			: (ID && !ID->SourceDisplayFileName.IsEmpty() ? ID->SourceDisplayFileName : TEXT("--"));
		SetText(ConvertWidgets,TEXT("TXT_RouteFilePath"),RoutePathText);
		SetTooltip(ConvertWidgets,TEXT("TXT_RouteFilePath"),RoutePathText);
		const FRoutePreviewData& Data = Preview->GetRouteData(); const bool Origin = Loaded && Data.bShowOriginSet;
		if (!Prepared && Origin)
		{
			SetEdit(ConvertWidgets,TEXT("EDT_OriginLatitude"),FString::Printf(TEXT("%.8f"),Data.ShowOriginLatitude));
			SetEdit(ConvertWidgets,TEXT("EDT_OriginLongitude"),FString::Printf(TEXT("%.8f"),Data.ShowOriginLongitude));
			SetEdit(ConvertWidgets,TEXT("EDT_OriginAltitude"),FString::Printf(TEXT("%.3f"),Data.ShowOriginAltitude));
		}
		FString PreviewText = Preview->IsPreviewModeActive() ? TEXT("Preview: ")+Playback(Preview->GetPlaybackState()) : (Preview->IsRouteOverlayVisible() ? TEXT("Live + Frame-0 Overlay") : (Preview->IsRouteOverlayEnabled() ? TEXT("Overlay Enabled") : TEXT("--")));
		SetText(ConvertWidgets,TEXT("TXT_StatePreviewValue"),PreviewText);
	}
	URouteArtifactWorkspaceSubsystem* Workspace = GI->GetSubsystem<URouteArtifactWorkspaceSubsystem>(); ERouteArtifactCurrentStatus C=ERouteArtifactCurrentStatus::None; ERouteArtifactGenerationStatus G=ERouteArtifactGenerationStatus::None; ERouteArtifactRecoveryStatus R=ERouteArtifactRecoveryStatus::Stable; FRouteArtifactCurrentInfo Info; FString Root;
	if (Workspace) { C=Workspace->GetCurrentStatus(); G=Workspace->GetGenerationStatus(); R=Workspace->GetLastRecoveryResult().Status; Info=Workspace->GetCurrentInfo(); FString Error; if(!Workspace->ResolveWorkspaceRoot(Root,Error)) Root.Reset(); }
	SetText(ConvertWidgets,TEXT("TXT_OutputPath"),Root.IsEmpty()?TEXT("--"):Root); SetTooltip(ConvertWidgets,TEXT("TXT_OutputPath"),Root.IsEmpty()?TEXT("--"):Root); const FString ReadyText=Ready(C,R); SetText(ConvertWidgets,TEXT("TXT_ArtifactReadyValue"),ReadyText); SetText(UploadWidgets,TEXT("TXT_ArtifactReadyValue"),ReadyText);
	SetText(ConvertWidgets,TEXT("TXT_ArtifactIdValue"),C==ERouteArtifactCurrentStatus::Ready&&!Info.ArtifactSetID.IsEmpty()?Info.ArtifactSetID:TEXT("--")); SetText(ConvertWidgets,TEXT("TXT_StateArtifactValue"),FString::Printf(TEXT("Current: %s / Generation: %s / Recovery: %s"),*Current(C),*Generation(G),*Recovery(R)));
	FRouteWaypointBatchProgress GP; if(const URouteWaypointGenerationSubsystem* Generator=GI->GetSubsystem<URouteWaypointGenerationSubsystem>()) GP=Generator->GetProgress();
	if (!GP.FirstFailureDiagnostic.IsEmpty()
		&& (GP.GenerationState == ERouteWaypointBatchState::Failed
			|| GP.GenerationState == ERouteWaypointBatchState::Cancelled))
	{
		SetText(ConvertWidgets,TEXT("TXT_StateArtifactValue"),GP.FirstFailureDiagnostic.Left(320));
	}
	const double GR=ComputeProgressRatio(FMath::Max(0,GP.CompletedRoutes),FMath::Max(0,GP.TotalRoutes)); if(UProgressBar* Bar=Get<UProgressBar>(ConvertWidgets,TEXT("PROGRESS_Convert"))) Bar->SetPercent(GR);
	SetText(ConvertWidgets,TEXT("TXT_GeneratedCount"),FString::Printf(TEXT("%d / %d"),GP.CompletedRoutes,GP.TotalRoutes)); SetText(ConvertWidgets,TEXT("TXT_ValidatedCount"),C==ERouteArtifactCurrentStatus::Ready?FString::Printf(TEXT("%d / %d"),Info.RouteSlotCount,Info.RouteSlotCount):TEXT("--")); SetText(ConvertWidgets,TEXT("TXT_ConvertPercent"),FormatPercent(GR));
	const bool HasElapsed=GP.GenerationState!=ERouteWaypointBatchState::Idle||GP.TotalRoutes>0||GP.ElapsedSeconds>0; SetText(ConvertWidgets,TEXT("TXT_ArtifactElapsedValue"),HasElapsed?FormatElapsedMilliseconds(FMath::Max(0.0,GP.ElapsedSeconds)*1000.0):TEXT("--"));
	const UDSPGCSUploadJobSubsystem* Upload=GI->GetSubsystem<UDSPGCSUploadJobSubsystem>(); const dspgcs::UploadJobSnapshotDTO* Job=Upload?Upload->GetStateMirror().GetSnapshot():nullptr;
	const auto Count=[this,Job](FName Name,uint32 Value){GCSRouteWorkflowPrivate::SetText(UploadWidgets,Name,Job?FString::Printf(TEXT("%u"),Value):TEXT("0"));};
	Count(TEXT("TXT_UploadMatched"),Job?Job->matchedCount:0); Count(TEXT("TXT_UploadVacant"),Job?Job->vacantCount:0); Count(TEXT("TXT_UploadSpare"),Job?Job->spareCount:0); Count(TEXT("TXT_UploadWaiting"),Job?Job->waitingCount:0); Count(TEXT("TXT_UploadPreparing"),Job?Job->preparingCount:0); Count(TEXT("TXT_UploadUploading"),Job?Job->uploadingCount:0); Count(TEXT("TXT_UploadRetryWaiting"),Job?Job->retryWaitingCount:0); Count(TEXT("TXT_UploadSucceeded"),Job?Job->succeededCount:0); Count(TEXT("TXT_UploadFailed"),Job?Job->failedCount:0); Count(TEXT("TXT_UploadCancelled"),Job?Job->cancelledCount:0);
	const UDSPGCSFrozenAssignmentSyncSubsystem* FrozenSync =
		GI->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
	FString ConsoleState = FrozenState(FrozenSync
		? FrozenSync->GetSyncState() : EDSPGCSFrozenAssignmentSyncState::NotFrozen);
	if (Job) ConsoleState += TEXT(" / Artifact: ") + Gate(Job->artifactGateState);
	else if (AutoStartState != EGCSRouteAutoStartState::Idle)
		ConsoleState += TEXT(" / Auto: ") + FormatAutoStartState(AutoStartState);
	SetText(UploadWidgets,TEXT("TXT_ConsoleValidatedValue"),ConsoleState);
	FString JobPresentation = Job
		? FormatUploadJobPresentation(static_cast<uint8>(Job->jobState))
		: (AutoStartState == EGCSRouteAutoStartState::Idle
			? TEXT("State: --") : TEXT("State: ") + FormatAutoStartState(AutoStartState));
	if (!Job && !AutoStartDiagnostic.IsEmpty())
		JobPresentation += TEXT(" / ") + AutoStartDiagnostic.Left(320);
	if (Job && Job->jobState == dspgcs::UploadJobState::Failed &&
		!Job->firstFailure.safeSummary.empty())
	{
		JobPresentation += TEXT(" / ") +
			WorkflowFromUtf8(Job->firstFailure.safeSummary).Left(320);
	}
	const FDSPGCSUploadJobCommandStatus CommandStatus =
		Upload ? Upload->GetCommandStatus() : FDSPGCSUploadJobCommandStatus{};
	if (!CommandStatus.LastSafeDiagnostic.IsEmpty() &&
		(CommandStatus.bCommandTimedOut || CommandStatus.bHasAuthoritativeFailure ||
		 CommandStatus.LastCommandResultCode !=
			 static_cast<int32>(dspgcs::UploadJobErrorCode::None) ||
		 (CommandStatus.bHasCommandAck && CommandStatus.LastCommandAckStatus ==
			 static_cast<int32>(dspgcs::UploadJobCommandAckStatus::Rejected))))
	{
		JobPresentation += TEXT(" / ") + CommandStatus.LastSafeDiagnostic.Left(320);
	}
	SetText(UploadWidgets,TEXT("TXT_UploadJobStateValue"),JobPresentation);
	const uint64 Total=Job?Job->totalBytes:0,Done=Job?Job->bytesCompleted:0; const double UR=ComputeProgressRatio(Done,Total); SetText(UploadWidgets,TEXT("TXT_UploadThroughputValue"),FormatThroughput(Job?Job->throughputBytesPerSecond:0)); SetText(UploadWidgets,TEXT("TXT_UploadBytesValue"),FormatLogicalByteProgress(Done,Total)); SetText(UploadWidgets,TEXT("TXT_UploadElapsedValue"),FormatElapsedMilliseconds(Job?Job->elapsedMs:0)); if(UProgressBar* Bar=Get<UProgressBar>(UploadWidgets,TEXT("PROGRESS_Upload"))) Bar->SetPercent(UR); SetText(UploadWidgets,TEXT("TXT_UploadPercent"),FormatPercent(UR)); SetText(UploadWidgets,TEXT("TXT_CurrentMaxConcurrencyValue"),Job?FString::Printf(TEXT("Current: %u"),Job->effectiveMaxConcurrentUploads):(AutoStartState!=EGCSRouteAutoStartState::Idle?TEXT("E.0 Start: 1"):TEXT("Current: --")));
	RefreshArtifactIdentity();
	SyncRouteListModel();
	RefreshVisibleRouteRows();
	UpdateCommandControlState();
	UpdateUploadCommandControlState();
	ApplyCommandErrorPresentation();
}

void UGCSRouteWorkflowCoordinator::RefreshArtifactIdentity()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;

	const UGameInstance* GI = GameInstanceRef.Get();
	const URouteArtifactWorkspaceSubsystem* Workspace =
		GI->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	const UDSPGCSUploadJobSubsystem* Upload =
		GI->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	const dspgcs::UploadJobSnapshotDTO* Job =
		Upload ? Upload->GetStateMirror().GetSnapshot() : nullptr;

	FString CurrentArtifactID;
	FString CurrentDigest;
	FString CurrentRoot;
	FString CurrentSource;
	bool bCurrentUsable = false;
	if (Workspace)
	{
		const ERouteArtifactRecoveryStatus RecoveryStatus =
			Workspace->GetLastRecoveryResult().Status;
		const FRouteArtifactCurrentInfo& Info = Workspace->GetCurrentInfo();
		const FRouteArtifactSourceIdentity* Source = Workspace->GetCurrentSourceIdentity();
		bCurrentUsable = Workspace->GetCurrentStatus() == ERouteArtifactCurrentStatus::Ready
			&& (RecoveryStatus == ERouteArtifactRecoveryStatus::Stable
				|| RecoveryStatus == ERouteArtifactRecoveryStatus::Recovered)
			&& Source != nullptr
			&& Source->IsValid()
			&& !Info.ArtifactSetID.IsEmpty()
			&& !Info.CurrentRoot.IsEmpty()
			&& !Source->SourceDisplayFileName.IsEmpty();
		if (bCurrentUsable)
		{
			CurrentArtifactID = Info.ArtifactSetID;
			CurrentDigest = Info.ArtifactSetDigest.ToLowerHex();
			CurrentRoot = Info.CurrentRoot;
			CurrentSource = Source->SourceDisplayFileName;
		}
	}

	const FGCSRouteUploadArtifactDisplay Display = ResolveArtifactDisplay(
		Job != nullptr,
		Job ? WorkflowFromUtf8(Job->artifactBinding.artifactSetId) : FString(),
		Job ? WorkflowDigestHex(Job->artifactBinding.artifactSetDigest) : FString(),
		bCurrentUsable,
		CurrentArtifactID,
		CurrentDigest,
		CurrentRoot,
		CurrentSource);
	bUploadArtifactMismatch = Display.bArtifactMismatch;

	const FString ArtifactText = Display.ArtifactSetID.Len() > 16
		? Display.ArtifactSetID.Left(12) + TEXT("...")
		: Display.ArtifactSetID;
	SetText(UploadWidgets, TEXT("TXT_UploadArtifactIdValue"),
		ArtifactText.IsEmpty() ? TEXT("--") : ArtifactText);
	SetTooltip(UploadWidgets, TEXT("TXT_UploadArtifactIdValue"),
		Display.ArtifactSetID.IsEmpty() ? TEXT("--") : Display.ArtifactSetID);
	SetText(UploadWidgets, TEXT("TXT_UploadWaypointsRootValue"),
		Display.WaypointsRoot.IsEmpty() ? TEXT("--") : Display.WaypointsRoot);
	SetTooltip(UploadWidgets, TEXT("TXT_UploadWaypointsRootValue"),
		Display.WaypointsRoot.IsEmpty() ? TEXT("--") : Display.WaypointsRoot);
	SetText(UploadWidgets, TEXT("TXT_UploadSourceDatasetValue"),
		Display.SourceDataset.IsEmpty() ? TEXT("--") : Display.SourceDataset);
	SetTooltip(UploadWidgets, TEXT("TXT_UploadSourceDatasetValue"),
		Display.SourceDataset.IsEmpty() ? TEXT("--") : Display.SourceDataset);

	if (bUploadArtifactMismatch)
	{
		SetText(UploadWidgets, TEXT("TXT_UploadJobStateValue"),
			Job ? FormatUploadJobPresentation(static_cast<uint8>(Job->jobState)) +
					TEXT(" / Artifact mismatch")
				: TEXT("State: --"));
	}
}

void UGCSRouteWorkflowCoordinator::SyncRouteListModel()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	const UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	const FDSPGCSUploadJobStateMirror* Mirror = Upload ? &Upload->GetStateMirror() : nullptr;
	const dspgcs::UploadJobSnapshotDTO* Job = Mirror ? Mirror->GetSnapshot() : nullptr;
	if (!Job)
	{
		if (!RouteListItems.IsEmpty() && RouteListItems[0] &&
			RouteListItems[0]->bAutoStartPreview &&
			AutoStartState != EGCSRouteAutoStartState::Idle)
		{
			return;
		}
		if (!RouteListItems.IsEmpty() || !RouteListJobID.IsEmpty()) ClearRouteListModel();
		return;
	}

	const FString JobID = WorkflowFromUtf8(Job->uploadJobId);
	const FString SnapshotToken = WorkflowFromUtf8(Job->snapshotToken);
	if (RouteListJobID != JobID || RouteListSnapshotToken != SnapshotToken)
	{
		ClearRouteListModel();
		RouteListJobID = JobID;
		RouteListSnapshotToken = SnapshotToken;
	}

	const bool bComplete = Mirror->IsRoutePageComplete()
		&& Mirror->GetRouteStateCount() == static_cast<int32>(Job->matchedCount);
	if (!bComplete)
	{
		const FString Presentation = FormatUploadJobPresentation(
			static_cast<uint8>(Job->jobState));
		SetText(UploadWidgets, TEXT("TXT_UploadJobStateValue"),
			bUploadArtifactMismatch
				? Presentation + TEXT(" / Routes: Loading / Artifact mismatch")
				: Presentation + TEXT(" / Routes: Loading"));
		return;
	}
	if (!RouteListItems.IsEmpty() || Job->matchedCount == 0) return;

	TArray<int32> SortedRouteSlots;
	Mirror->CopySortedRouteSlots(SortedRouteSlots);
	if (SortedRouteSlots.Num() != static_cast<int32>(Job->matchedCount))
	{
		SetText(UploadWidgets, TEXT("TXT_UploadJobStateValue"),
			FormatUploadJobPresentation(static_cast<uint8>(Job->jobState)) +
				TEXT(" / Routes: Loading"));
		return;
	}
	ReplaceRouteListItems(JobID, SnapshotToken, SortedRouteSlots);
}

bool UGCSRouteWorkflowCoordinator::ReplaceRouteListItems(
	const FString& JobID,
	const FString& SnapshotToken,
	const TArray<int32>& SortedRouteSlots)
{
	if (!IsInGameThread()) return false;
	if (RouteListJobID == JobID && RouteListSnapshotToken == SnapshotToken
		&& RouteListItems.Num() == SortedRouteSlots.Num())
	{
		bool bSame = true;
		for (int32 Index = 0; Index < SortedRouteSlots.Num(); ++Index)
		{
			if (!RouteListItems[Index]
				|| RouteListItems[Index]->RouteSlotIndex != SortedRouteSlots[Index])
			{
				bSame = false;
				break;
			}
		}
		if (bSame) return true;
	}
	for (int32 Index = 0; Index < SortedRouteSlots.Num(); ++Index)
	{
		if (SortedRouteSlots[Index] < 0
			|| (Index > 0 && SortedRouteSlots[Index - 1] >= SortedRouteSlots[Index]))
		{
			return false;
		}
	}

	ClearRouteListModel();
	RouteListJobID = JobID;
	RouteListSnapshotToken = SnapshotToken;
	RouteListItems.Reserve(SortedRouteSlots.Num());
	RouteItemsBySlot.Reserve(SortedRouteSlots.Num());
	TArray<UObject*> ListItems;
	ListItems.Reserve(SortedRouteSlots.Num());
	for (const int32 RouteSlot : SortedRouteSlots)
	{
		UGCSRouteUploadListItem* Item = NewObject<UGCSRouteUploadListItem>(this);
		Item->RouteSlotIndex = RouteSlot;
		Item->UploadJobID = JobID;
		Item->SnapshotToken = SnapshotToken;
		RouteListItems.Add(Item);
		RouteItemsBySlot.Add(RouteSlot, Item);
		ListItems.Add(Item);
	}
	if (UListView* List = GCSRouteWorkflowPrivate::Get<UListView>(
		UploadWidgets, TEXT("LIST_RouteUploadStates")))
	{
		List->SetListItems(ListItems);
	}
	++RouteListRebuildCount;
	return true;
}

void UGCSRouteWorkflowCoordinator::ClearRouteListModel()
{
	if (UListView* List = GCSRouteWorkflowPrivate::Get<UListView>(
		UploadWidgets, TEXT("LIST_RouteUploadStates")))
	{
		List->ClearListItems();
	}
	RouteListItems.Reset();
	RouteItemsBySlot.Reset();
	RouteListJobID.Reset();
	RouteListSnapshotToken.Reset();
	ValidatedRouteRows.Reset();
	RouteRowDiagnostics.Reset();
}

bool UGCSRouteWorkflowCoordinator::ReplaceAutoStartPreviewItems(
	const FGCSRouteAutoStartSnapshot& Snapshot)
{
	if (!IsInGameThread() || Snapshot.Targets.IsEmpty()) return false;
	ClearRouteListModel();
	RouteListJobID = TEXT("AUTO:") + Snapshot.TransactionID;
	RouteListSnapshotToken = Snapshot.TransactionID;
	TArray<UObject*> ListItems;
	RouteListItems.Reserve(Snapshot.Targets.Num());
	ListItems.Reserve(Snapshot.Targets.Num());
	for (const FGCSRouteAutoStartTargetSnapshot& Target : Snapshot.Targets)
	{
		UGCSRouteUploadListItem* Item = NewObject<UGCSRouteUploadListItem>(this);
		Item->RouteSlotIndex = Target.RouteSlotIndex;
		Item->UploadJobID = RouteListJobID;
		Item->SnapshotToken = RouteListSnapshotToken;
		Item->bAutoStartPreview = true;
		Item->PreviewDroneID = Target.DroneID;
		Item->PreviewRouteRelativePath = Target.RouteRelativePath;
		RouteListItems.Add(Item);
		RouteItemsBySlot.Add(Target.RouteSlotIndex, Item);
		ListItems.Add(Item);
	}
	if (UListView* List = GCSRouteWorkflowPrivate::Get<UListView>(
		UploadWidgets, TEXT("LIST_RouteUploadStates")))
	{
		List->SetListItems(ListItems);
	}
	++RouteListRebuildCount;
	return true;
}

void UGCSRouteWorkflowCoordinator::RefreshVisibleRouteRows()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !GameInstanceRef.IsValid() || RouteListItems.IsEmpty()) return;
	UListView* List = Get<UListView>(UploadWidgets, TEXT("LIST_RouteUploadStates"));
	const UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	const URouteArtifactWorkspaceSubsystem* Workspace =
		GameInstanceRef->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	if (!List) return;
	const FDSPGCSUploadJobStateMirror* Mirror = Upload ? &Upload->GetStateMirror() : nullptr;

	for (UUserWidget* Row : List->GetDisplayedEntryWidgets())
	{
		if (!Row || !Row->WidgetTree) continue;
		TArray<FString> Issues;
		if (!ValidatedRouteRows.Contains(Row))
		{
			const FString StructureDiagnostic = DiagnoseRouteRowStructure(Row);
			StructureDiagnostic.ParseIntoArray(Issues, TEXT(", "), true);
		}

		UObject* ListObject = List->GetListObjectFromEntry(*Row);
		if (!ListObject) Issues.Add(TEXT("ListItemMissing"));
		const UGCSRouteUploadListItem* Item = Cast<UGCSRouteUploadListItem>(ListObject);
		if (ListObject && !Item) Issues.Add(TEXT("WrongListItemType"));
		if (!Issues.IsEmpty())
		{
			const FString Diagnostic = FString::Join(Issues, TEXT(", "));
			const FString* Previous = RouteRowDiagnostics.Find(Row);
			if (!Previous || *Previous != Diagnostic)
			{
				UE_LOG(LogGCSRouteWorkflow, Error,
					TEXT("Route upload row binding invalid: %s"), *Diagnostic);
				RouteRowDiagnostics.Add(Row, Diagnostic);
			}
			if (UTextBlock* Text = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Route"))))
				Text->SetText(FText::FromString(TEXT("--")));
			if (UTextBlock* Text = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Drone"))))
				Text->SetText(FText::FromString(TEXT("--")));
			if (UTextBlock* Text = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_State"))))
				Text->SetText(FText::FromString(TEXT("Error")));
			if (UTextBlock* Text = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Progress"))))
				Text->SetText(FText::FromString(TEXT("--")));
			if (UTextBlock* Text = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Retry"))))
				Text->SetText(FText::FromString(TEXT("--")));
			continue;
		}
		ValidatedRouteRows.Add(Row);
		RouteRowDiagnostics.Remove(Row);
		if (!Item || Item->UploadJobID != RouteListJobID
			|| Item->SnapshotToken != RouteListSnapshotToken) continue;
		if (Item->bAutoStartPreview)
		{
			UTextBlock* RouteText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Route")));
			UTextBlock* DroneText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Drone")));
			UTextBlock* StateText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_State")));
			UTextBlock* ProgressText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Progress")));
			UTextBlock* RetryText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Retry")));
			RouteText->SetText(FText::FromString(FString::Printf(
				TEXT("Slot %d / %s"), Item->RouteSlotIndex,
				*FormatRouteFileDisplay(Item->PreviewRouteRelativePath))));
			RouteText->SetToolTipText(FText::FromString(Item->PreviewRouteRelativePath));
			DroneText->SetText(FText::FromString(Item->PreviewDroneID));
			StateText->SetText(FText::FromString(FormatAutoStartState(AutoStartState)));
			StateText->SetToolTipText(FText::FromString(AutoStartDiagnostic));
			ProgressText->SetText(FText::FromString(TEXT("--")));
			RetryText->SetText(FText::FromString(TEXT("0")));
			continue;
		}
		if (!Mirror) continue;
		const dspgcs::UploadRouteStateDTO* State = Mirror->FindRoute(Item->RouteSlotIndex);
		if (!State) continue;

		FRouteArtifactRecord Record;
		const bool bHasRecord = !bUploadArtifactMismatch && Workspace
			&& Workspace->FindCurrentRouteRecord(Item->RouteSlotIndex, Record);
		const FString FullRoute = bHasRecord ? Record.RelativePath : TEXT("--");
		UTextBlock* RouteText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Route")));
		UTextBlock* DroneText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Drone")));
		UTextBlock* StateText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_State")));
		UTextBlock* ProgressText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Progress")));
		UTextBlock* RetryText = Cast<UTextBlock>(Row->WidgetTree->FindWidget(TEXT("TXT_Retry")));
		RouteText->SetText(FText::FromString(bHasRecord ? FormatRouteFileDisplay(FullRoute) : TEXT("--")));
		RouteText->SetToolTipText(FText::FromString(FullRoute));
		const FString DroneID = WorkflowFromUtf8(State->droneId);
		DroneText->SetText(FText::FromString(DroneID.IsEmpty() ? TEXT("--") : DroneID));
		FString RouteStateText = FormatRouteState(static_cast<uint8>(State->state));
		if (State->state == dspgcs::UploadRouteState::Failed &&
			State->errorCode != dspgcs::RouteUploadErrorCode::None)
		{
			RouteStateText += TEXT(" / ") +
				FormatRouteErrorCode(static_cast<uint16>(State->errorCode));
		}
		StateText->SetText(FText::FromString(RouteStateText));
		StateText->SetToolTipText(FText::FromString(
			State->safeFailureSummary.empty()
				? RouteStateText : WorkflowFromUtf8(State->safeFailureSummary).Left(512)));
		ProgressText->SetText(FText::FromString(FormatPercent(
			ComputeProgressRatio(State->bytesCompleted, State->totalBytes))));
		RetryText->SetText(FText::AsNumber(ComputeRetryCount(State->attemptCount)));
	}
}

bool UGCSRouteWorkflowCoordinator::TickRefresh(float DeltaSeconds) { (void)DeltaSeconds; AdvanceAutoStartTransaction(); if(RouteWorkflowWidget&&RouteWorkflowWidget->IsVisible()) RefreshReadOnlyState(); return true; }
void UGCSRouteWorkflowCoordinator::StartRefreshTicker() { if(!RefreshTickerHandle.IsValid()) RefreshTickerHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this,&UGCSRouteWorkflowCoordinator::TickRefresh),RefreshIntervalSeconds); }
void UGCSRouteWorkflowCoordinator::StopRefreshTicker() { if(RefreshTickerHandle.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle); RefreshTickerHandle.Reset(); } }
double UGCSRouteWorkflowCoordinator::ComputeProgressRatio(uint64 Completed,uint64 Total) { return Total==0?0.0:FMath::Clamp(static_cast<double>(Completed)/Total,0.0,1.0); }
FString UGCSRouteWorkflowCoordinator::FormatByteCount(uint64 Bytes) { using namespace GCSRouteWorkflowPrivate; constexpr double KB=1024.0,MB=KB*1024.0,GB=MB*1024.0; if(Bytes<1024)return FString::Printf(TEXT("%llu B"),Bytes); if(Bytes<static_cast<uint64>(MB))return Compact(Bytes/KB)+TEXT(" KB"); if(Bytes<static_cast<uint64>(GB))return Compact(Bytes/MB)+TEXT(" MB"); return Compact(Bytes/GB)+TEXT(" GB"); }
FString UGCSRouteWorkflowCoordinator::FormatLogicalByteProgress(uint64 Completed,uint64 Total) { return FString::Printf(TEXT("%llu B / %llu B (logical payload)"),Completed,Total); }
FString UGCSRouteWorkflowCoordinator::FormatThroughput(uint64 Value) { return FormatByteCount(Value)+TEXT("/s (avg logical payload)"); }
FString UGCSRouteWorkflowCoordinator::FormatElapsedMilliseconds(uint64 Value) { const uint64 S=Value/1000; return FString::Printf(TEXT("%02llu:%02llu:%02llu.%03llu"),S/3600,(S/60)%60,S%60,Value%1000); }
FString UGCSRouteWorkflowCoordinator::FormatUploadJobPresentation(uint8 JobStateValue) { using namespace GCSRouteWorkflowPrivate; return TEXT("State: ")+JobState(static_cast<dspgcs::UploadJobState>(JobStateValue)); }
FString UGCSRouteWorkflowCoordinator::FormatPercent(double Value) { return FString::Printf(TEXT("%.0f%%"),FMath::Clamp(Value,0.0,1.0)*100.0); }
int32 UGCSRouteWorkflowCoordinator::ComputeRetryCount(const uint32 AttemptCount)
{
	return AttemptCount <= 1 ? 0 : static_cast<int32>(FMath::Min<uint32>(
		AttemptCount - 1, static_cast<uint32>(MAX_int32)));
}
FString UGCSRouteWorkflowCoordinator::FormatRouteState(const uint8 RouteStateValue)
{
	switch (static_cast<dspgcs::UploadRouteState>(RouteStateValue))
	{
	case dspgcs::UploadRouteState::Waiting: return TEXT("Waiting");
	case dspgcs::UploadRouteState::Preparing: return TEXT("Preparing");
	case dspgcs::UploadRouteState::Uploading: return TEXT("Protocol Uploading");
	case dspgcs::UploadRouteState::RetryWaiting: return TEXT("Retry Waiting");
	case dspgcs::UploadRouteState::Succeeded: return TEXT("Protocol Succeeded");
	case dspgcs::UploadRouteState::Failed: return TEXT("Failed");
	case dspgcs::UploadRouteState::Cancelled: return TEXT("Cancelled");
	default: return TEXT("--");
	}
}
FString UGCSRouteWorkflowCoordinator::FormatRouteErrorCode(const uint16 ErrorCodeValue)
{
	switch (static_cast<dspgcs::RouteUploadErrorCode>(ErrorCodeValue))
	{
	case dspgcs::RouteUploadErrorCode::None: return TEXT("None");
	case dspgcs::RouteUploadErrorCode::RouteDescriptorMissing: return TEXT("RouteDescriptorMissing");
	case dspgcs::RouteUploadErrorCode::RouteFileUnavailable: return TEXT("RouteFileUnavailable");
	case dspgcs::RouteUploadErrorCode::RouteIdentityMismatch: return TEXT("RouteIdentityMismatch");
	case dspgcs::RouteUploadErrorCode::WaypointValidationFailed: return TEXT("WaypointValidationFailed");
	case dspgcs::RouteUploadErrorCode::MissionEncodingFailed: return TEXT("MissionEncodingFailed");
	case dspgcs::RouteUploadErrorCode::TransportUnavailable: return TEXT("TransportUnavailable");
	case dspgcs::RouteUploadErrorCode::UploadRejected: return TEXT("UploadRejected");
	case dspgcs::RouteUploadErrorCode::UploadTimedOut: return TEXT("UploadTimedOut");
	case dspgcs::RouteUploadErrorCode::Cancelled: return TEXT("Cancelled");
	case dspgcs::RouteUploadErrorCode::InternalError: return TEXT("InternalError");
	default: return TEXT("Unknown");
	}
}
FString UGCSRouteWorkflowCoordinator::DiagnoseRouteRowStructure(const UUserWidget* Row)
{
	if (!Row || !Row->WidgetTree) return TEXT("RowMissing");
	TArray<FString> Issues;
	if (!Row->Implements<UUserObjectListEntry>()) Issues.Add(TEXT("InterfaceMissing"));
	for (const FName Name : {FName(TEXT("TXT_Route")), FName(TEXT("TXT_Drone")),
		FName(TEXT("TXT_State")), FName(TEXT("TXT_Progress")), FName(TEXT("TXT_Retry"))})
	{
		UWidget* Widget = Row->WidgetTree->FindWidget(Name);
		if (!Widget) Issues.Add(Name.ToString() + TEXT(" Missing"));
		else if (!Cast<UTextBlock>(Widget)) Issues.Add(Name.ToString() + TEXT(" WrongType"));
	}
	return FString::Join(Issues, TEXT(", "));
}
FString UGCSRouteWorkflowCoordinator::FormatRouteFileDisplay(
	const FString& ManifestRelativePath)
{
	return ManifestRelativePath.IsEmpty()
		? TEXT("--")
		: FPaths::GetCleanFilename(ManifestRelativePath);
}
FGCSRouteUploadArtifactDisplay UGCSRouteWorkflowCoordinator::ResolveArtifactDisplay(
	const bool bHasJob,
	const FString& JobArtifactSetID,
	const FString& JobArtifactDigest,
	const bool bCurrentUsable,
	const FString& CurrentArtifactSetID,
	const FString& CurrentArtifactDigest,
	const FString& CurrentRoot,
	const FString& CurrentSourceDataset)
{
	FGCSRouteUploadArtifactDisplay Result;
	if (!bHasJob)
	{
		if (bCurrentUsable)
		{
			Result.ArtifactSetID = CurrentArtifactSetID;
			Result.WaypointsRoot = CurrentRoot;
			Result.SourceDataset = CurrentSourceDataset;
		}
		return Result;
	}
	Result.ArtifactSetID = JobArtifactSetID;
	const bool bExactCurrent = bCurrentUsable
		&& JobArtifactSetID == CurrentArtifactSetID
		&& JobArtifactDigest == CurrentArtifactDigest;
	if (bExactCurrent)
	{
		Result.WaypointsRoot = CurrentRoot;
		Result.SourceDataset = CurrentSourceDataset;
	}
	else
	{
		Result.bArtifactMismatch = true;
	}
	return Result;
}
bool UGCSRouteWorkflowCoordinator::ParseOriginInputs(
	const FString& LatitudeText,
	const FString& LongitudeText,
	const FString& AltitudeText,
	double& OutLatitude,
	double& OutLongitude,
	double& OutAltitude,
	FString& OutError)
{
	using namespace GCSRouteWorkflowPrivate;
	OutLatitude = 0.0;
	OutLongitude = 0.0;
	OutAltitude = 0.0;
	OutError.Reset();
	if (!WorkflowParseFiniteDouble(LatitudeText, OutLatitude)
		|| OutLatitude < -90.0 || OutLatitude > 90.0)
	{
		OutError = TEXT("Latitude must be a complete finite number in [-90,90].");
		return false;
	}
	if (!WorkflowParseFiniteDouble(LongitudeText, OutLongitude)
		|| OutLongitude < -180.0 || OutLongitude > 180.0)
	{
		OutError = TEXT("Longitude must be a complete finite number in [-180,180].");
		return false;
	}
	if (!WorkflowParseFiniteDouble(AltitudeText, OutAltitude))
	{
		OutError = TEXT("Altitude must be a complete finite number.");
		return false;
	}
	return true;
}
FGCSRouteWorkflowCommandGate UGCSRouteWorkflowCoordinator::ComputeCommandGate(
	const FGCSRouteWorkflowCommandGateInputs& Inputs)
{
	FGCSRouteWorkflowCommandGate Gate;
	const bool bMutationBlocked = Inputs.bGenerationActive || Inputs.bCommandInFlight;
	Gate.bRouteBrowseEnabled = !bMutationBlocked;
	Gate.bOutputBrowseEnabled = !bMutationBlocked;
	Gate.bLoadPreviewEnabled = !bMutationBlocked
		&& Inputs.bHasPreparedRoute && Inputs.bOriginValid;
	Gate.bGenerateWaypointsEnabled = !bMutationBlocked
		&& !Inputs.bHasPreparedRoute
		&& Inputs.bRouteLoaded
		&& Inputs.bShowOriginSet
		&& Inputs.bWorkspaceReady;
	return Gate;
}

bool UGCSRouteWorkflowCoordinator::ParsePositiveConcurrency(
	const FString& Text,
	int32& OutValue,
	FString& OutError)
{
	OutValue = 0;
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("Maximum concurrency is required.");
		return false;
	}
	uint64 Value = 0;
	for (const TCHAR Character : Trimmed)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			OutError = TEXT("Maximum concurrency must be a positive integer.");
			return false;
		}
		Value = Value * 10 + static_cast<uint64>(Character - TEXT('0'));
		if (Value > static_cast<uint64>(MAX_int32))
		{
			OutError = TEXT("Maximum concurrency is too large.");
			return false;
		}
	}
	if (Value == 0)
	{
		OutError = TEXT("Maximum concurrency must be greater than zero.");
		return false;
	}
	OutValue = static_cast<int32>(Value);
	OutError.Reset();
	return true;
}

FGCSRouteUploadCommandGate UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(
	const FGCSRouteUploadCommandGateInputs& Inputs)
{
	FGCSRouteUploadCommandGate Gate;
	Gate.StartBlockedReason = Inputs.StartBlockedReason;
	if (Inputs.bCommandInFlight) return Gate;
	if (!Inputs.bHasActiveJobBinding && !Inputs.bHasExecutableJob)
	{
		Gate.bStartEnabled =
			Inputs.bArtifactReady && Inputs.bAutoStartMappingValid &&
			!Inputs.bAutoStartInFlight;
		if (Gate.bStartEnabled) Gate.StartBlockedReason.Reset();
		return Gate;
	}
	if (!Inputs.bHasExecutableJob) return Gate;
	switch (static_cast<dspgcs::UploadJobState>(Inputs.JobState))
	{
	case dspgcs::UploadJobState::Ready:
	case dspgcs::UploadJobState::Running:
		Gate.bPauseEnabled = true;
		Gate.bCancelEnabled = true;
		Gate.bApplyMaxEnabled = true;
		break;
	case dspgcs::UploadJobState::Paused:
		Gate.bResumeEnabled = true;
		Gate.bCancelEnabled = true;
		Gate.bApplyMaxEnabled = true;
		break;
	case dspgcs::UploadJobState::Failed:
		Gate.bCancelEnabled = true;
		Gate.bRetryFailedEnabled = Inputs.FailedCount > 0;
		Gate.bApplyMaxEnabled = true;
		break;
	default:
		break;
	}
	return Gate;
}

FString UGCSRouteWorkflowCoordinator::FormatAutoStartState(
	const EGCSRouteAutoStartState State)
{
	switch (State)
	{
	case EGCSRouteAutoStartState::ValidatingMapping: return TEXT("Validating Mapping");
	case EGCSRouteAutoStartState::Freezing: return TEXT("Freezing Assignment");
	case EGCSRouteAutoStartState::SyncingConsole: return TEXT("Syncing Console");
	case EGCSRouteAutoStartState::StartingJob: return TEXT("Starting Upload");
	case EGCSRouteAutoStartState::Completed: return TEXT("Upload Job Started");
	case EGCSRouteAutoStartState::Failed: return TEXT("Automatic Start Failed");
	case EGCSRouteAutoStartState::CancelledAsStale: return TEXT("Automatic Start Stale");
	default: return TEXT("Idle");
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void UGCSRouteWorkflowCoordinator::InitializeForTest(UGameInstance* GI) { Shutdown(); GameInstanceRef=GI; }
void UGCSRouteWorkflowCoordinator::SetWorkflowFactoryForTest(TFunction<UUserWidget*()> Factory,bool bSkip) { WorkflowFactoryForTest=MoveTemp(Factory); bSkipRequiredWidgetBindingForTest=bSkip; }
bool UGCSRouteWorkflowCoordinator::BuildRouteItemsForTest(
	const FString& JobID,
	const FString& SnapshotToken,
	const TArray<int32>& SortedRouteSlots)
{
	return ReplaceRouteListItems(JobID, SnapshotToken, SortedRouteSlots);
}
const UGCSRouteUploadListItem* UGCSRouteWorkflowCoordinator::FindRouteItemForTest(
	const int32 RouteSlotIndex) const
{
	const TObjectPtr<UGCSRouteUploadListItem>* Item = RouteItemsBySlot.Find(RouteSlotIndex);
	return Item ? Item->Get() : nullptr;
}
uint64 UGCSRouteWorkflowCoordinator::EstimateRouteItemModelBytes(const int32 ItemCount)
{
	if (ItemCount <= 0) return 0;
	return static_cast<uint64>(ItemCount)
		* static_cast<uint64>(sizeof(UGCSRouteUploadListItem)
			+ sizeof(TObjectPtr<UGCSRouteUploadListItem>)
			+ sizeof(TPair<int32, TObjectPtr<UGCSRouteUploadListItem>>));
}
#endif
