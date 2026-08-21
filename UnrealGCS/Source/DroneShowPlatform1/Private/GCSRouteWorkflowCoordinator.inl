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
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceSubsystem.h"
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
		switch (V) { case dspgcs::UploadJobState::Created:return TEXT("Created"); case dspgcs::UploadJobState::Validating:return TEXT("Validating"); case dspgcs::UploadJobState::Ready:return TEXT("Ready"); case dspgcs::UploadJobState::Running:return TEXT("Running"); case dspgcs::UploadJobState::Paused:return TEXT("Paused"); case dspgcs::UploadJobState::Cancelling:return TEXT("Cancelling"); case dspgcs::UploadJobState::Completed:return TEXT("Completed"); case dspgcs::UploadJobState::Failed:return TEXT("Failed"); case dspgcs::UploadJobState::Cancelled:return TEXT("Cancelled"); default:return TEXT("--"); }
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
	ConvertWidgets.Reset(); UploadWidgets.Reset();
	OpenWorkflowButtonRef.Reset(); RightPanelWidgetRef.Reset(); GameInstanceRef.Reset();
	bBindingValid = false;
	bUploadArtifactMismatch = false;
	SelectedRouteImportFilePath.Reset();
	SelectedRouteSourceDigest.Reset();
	CommandErrorMessage.Reset();
	CommandErrorExpiresAtSeconds = 0.0;
	CommandErrorCategory = EGCSRouteWorkflowCommandErrorCategory::None;
	bCommandInFlight = false;
#if WITH_DEV_AUTOMATION_TESTS
	WorkflowFactoryForTest = {};
	bSkipRequiredWidgetBindingForTest = false;
	CommandTestHooks = {};
	CommandGateInputsForTest.Reset();
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
	RouteWorkflowWidget->SetVisibility(ESlateVisibility::Visible);
	RefreshReadOnlyState();
#if WITH_DEV_AUTOMATION_TESTS
	if (!bTest)
#endif
	if (UDSPGCSUploadJobSubsystem* Upload = GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>()) Upload->NotifyUploadPageOpened();
	StartRefreshTicker();
	return true;
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
	RouteConvertWidgetRef = Convert; RouteUploadWidgetRef = Upload;
	ClearRouteListModel();
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
	URoutePreviewSubsystem* Preview = GameInstanceRef->GetSubsystem<URoutePreviewSubsystem>();
	const FGCSRouteWorkflowCommandGateInputs Inputs = GatherCommandGateInputs();
	if (Inputs.bGenerationActive || !Inputs.bHasPreparedRoute) return;

	double Latitude = 0.0;
	double Longitude = 0.0;
	double Altitude = 0.0;
	FString Error;
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
		const bool bCommitted = CommandTestHooks.CommitRoute
			? CommandTestHooks.CommitRoute(Error)
			: (Preview && Preview->CommitPreparedRoute(Error));
		const bool bOriginSet = bCommitted && (CommandTestHooks.SetOrigin
			? CommandTestHooks.SetOrigin(Latitude, Longitude, Altitude, Error)
			: (Preview && Preview->SetShowOrigin(Latitude, Longitude, Altitude, Error)));
		bSucceeded = bOriginSet && (CommandTestHooks.SetOverlay
			? CommandTestHooks.SetOverlay(true, Error)
			: (Preview && Preview->SetRouteOverlayEnabled(true, Error)));
#else
		bSucceeded = Preview
			&& Preview->CommitPreparedRoute(Error)
			&& Preview->SetShowOrigin(Latitude, Longitude, Altitude, Error)
			&& Preview->SetRouteOverlayEnabled(true, Error);
#endif
	}
	if (bSucceeded)
	{
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
	int32 Maximum = 0;
	FString Error;
	if (!ReadConcurrencyDraft(Maximum, Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
		return;
	}
	UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	FString UploadJobID;
	if (!Upload || !Upload->StartUploadJobFromCurrentAuthorities(
		Maximum, UploadJobID, Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Upload Job Start was rejected.");
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload, Error);
	}
	else
	{
		ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
	}
	RefreshReadOnlyState();
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
	Inputs.bCommandInFlight = bCommandInFlight;
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
	if (!Upload) return Inputs;
	const FDSPGCSUploadJobCommandStatus Command = Upload->GetCommandStatus();
	Inputs.bHasActiveJobBinding = Command.bHasActiveJobBinding;
	Inputs.bHasExecutableJob = Command.bHasExecutableJob;
	Inputs.bCommandInFlight = Command.bCommandInFlight;
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
		Button->SetIsEnabled(Gate.bStartEnabled);
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
	SetText(UploadWidgets,TEXT("TXT_ConsoleValidatedValue"),ConsoleState);
	FString JobPresentation = Job
		? TEXT("Protocol: ") + JobState(Job->jobState) : TEXT("Protocol: --");
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
	const uint64 Total=Job?Job->totalBytes:0,Done=Job?Job->bytesCompleted:0; const double UR=ComputeProgressRatio(Done,Total); SetText(UploadWidgets,TEXT("TXT_UploadThroughputValue"),FormatThroughput(Job?Job->throughputBytesPerSecond:0)); SetText(UploadWidgets,TEXT("TXT_UploadBytesValue"),FString::Printf(TEXT("%s / %s"),*FormatByteCount(Done),*FormatByteCount(Total))); SetText(UploadWidgets,TEXT("TXT_UploadElapsedValue"),FormatElapsedMilliseconds(Job?Job->elapsedMs:0)); if(UProgressBar* Bar=Get<UProgressBar>(UploadWidgets,TEXT("PROGRESS_Upload"))) Bar->SetPercent(UR); SetText(UploadWidgets,TEXT("TXT_UploadPercent"),FormatPercent(UR)); SetText(UploadWidgets,TEXT("TXT_CurrentMaxConcurrencyValue"),Job?FString::Printf(TEXT("Current: %u"),Job->effectiveMaxConcurrentUploads):TEXT("Current: --"));
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
			Job ? TEXT("Protocol: ") + JobState(Job->jobState) + TEXT(" / Artifact mismatch")
				: TEXT("Protocol: --"));
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
		SetText(UploadWidgets, TEXT("TXT_UploadJobStateValue"),
			bUploadArtifactMismatch
				? TEXT("Protocol: Loading Routes / Artifact mismatch")
				: TEXT("Protocol: Loading Routes"));
		return;
	}
	if (!RouteListItems.IsEmpty() || Job->matchedCount == 0) return;

	TArray<int32> SortedRouteSlots;
	Mirror->CopySortedRouteSlots(SortedRouteSlots);
	if (SortedRouteSlots.Num() != static_cast<int32>(Job->matchedCount))
	{
		SetText(UploadWidgets, TEXT("TXT_UploadJobStateValue"),
			TEXT("Protocol: Loading Routes"));
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

void UGCSRouteWorkflowCoordinator::RefreshVisibleRouteRows()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !GameInstanceRef.IsValid() || RouteListItems.IsEmpty()) return;
	UListView* List = Get<UListView>(UploadWidgets, TEXT("LIST_RouteUploadStates"));
	const UDSPGCSUploadJobSubsystem* Upload =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	const URouteArtifactWorkspaceSubsystem* Workspace =
		GameInstanceRef->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	if (!List || !Upload) return;
	const FDSPGCSUploadJobStateMirror& Mirror = Upload->GetStateMirror();

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
		const dspgcs::UploadRouteStateDTO* State = Mirror.FindRoute(Item->RouteSlotIndex);
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

bool UGCSRouteWorkflowCoordinator::TickRefresh(float DeltaSeconds) { (void)DeltaSeconds; if(RouteWorkflowWidget&&RouteWorkflowWidget->IsVisible()) RefreshReadOnlyState(); return true; }
void UGCSRouteWorkflowCoordinator::StartRefreshTicker() { if(!RefreshTickerHandle.IsValid()) RefreshTickerHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this,&UGCSRouteWorkflowCoordinator::TickRefresh),RefreshIntervalSeconds); }
void UGCSRouteWorkflowCoordinator::StopRefreshTicker() { if(RefreshTickerHandle.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle); RefreshTickerHandle.Reset(); } }
double UGCSRouteWorkflowCoordinator::ComputeProgressRatio(uint64 Completed,uint64 Total) { return Total==0?0.0:FMath::Clamp(static_cast<double>(Completed)/Total,0.0,1.0); }
FString UGCSRouteWorkflowCoordinator::FormatByteCount(uint64 Bytes) { using namespace GCSRouteWorkflowPrivate; constexpr double KB=1024.0,MB=KB*1024.0,GB=MB*1024.0; if(Bytes<1024)return FString::Printf(TEXT("%llu B"),Bytes); if(Bytes<static_cast<uint64>(MB))return Compact(Bytes/KB)+TEXT(" KB"); if(Bytes<static_cast<uint64>(GB))return Compact(Bytes/MB)+TEXT(" MB"); return Compact(Bytes/GB)+TEXT(" GB"); }
FString UGCSRouteWorkflowCoordinator::FormatThroughput(uint64 Value) { return FormatByteCount(Value)+TEXT("/s"); }
FString UGCSRouteWorkflowCoordinator::FormatElapsedMilliseconds(uint64 Value) { const uint64 S=Value/1000; return FString::Printf(TEXT("%02llu:%02llu:%02llu"),S/3600,(S/60)%60,S%60); }
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
	if (Inputs.bCommandInFlight) return Gate;
	if (!Inputs.bHasActiveJobBinding && !Inputs.bHasExecutableJob)
	{
		Gate.bStartEnabled =
			Inputs.bArtifactReady && Inputs.bFrozenConsoleAccepted;
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
