#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "DSPGCSUploadJobSubsystem.h"
#include "DroneFleetVisualizationActor.h"
#include "DroneLogicalId.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FleetManagerSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "GCSUIBridgeSubsystem.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceSubsystem.h"
#include "RouteAssignmentPipeline.h"
#include "RouteAssignmentSubsystem.h"
#include "RouteGpsCaptureSubsystem.h"
#include "RoutePreviewSubsystem.h"
#include "RouteWaypointGenerator.h"
#include "RouteWaypointGenerationSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogGCSRouteWorkflow, Log, All);

namespace GCSRouteWorkflowPrivate
{
	FString GpsCaptureState(const ERouteGpsCaptureState State)
	{
		switch (State)
		{
		case ERouteGpsCaptureState::Idle: return TEXT("Not Captured");
		case ERouteGpsCaptureState::Capturing: return TEXT("Capturing");
		case ERouteGpsCaptureState::Ready: return TEXT("Ready");
		case ERouteGpsCaptureState::Failed: return TEXT("Failed");
		case ERouteGpsCaptureState::Invalidated: return TEXT("Invalidated");
		default: return TEXT("Unknown");
		}
	}

	FString GpsCaptureFailure(const ERouteGpsCaptureFailureReason Reason)
	{
		switch (Reason)
		{
		case ERouteGpsCaptureFailureReason::None: return TEXT("None");
		case ERouteGpsCaptureFailureReason::Offline: return TEXT("Offline");
		case ERouteGpsCaptureFailureReason::ParticipantMissing: return TEXT("ParticipantMissing");
		case ERouteGpsCaptureFailureReason::EndpointChanged: return TEXT("EndpointChanged");
		case ERouteGpsCaptureFailureReason::FleetSessionChanged: return TEXT("FleetSessionChanged");
		case ERouteGpsCaptureFailureReason::GpsUnavailable: return TEXT("GpsUnavailable");
		case ERouteGpsCaptureFailureReason::GpsStale: return TEXT("GpsStale");
		case ERouteGpsCaptureFailureReason::FixQualityInsufficient: return TEXT("FixQualityInsufficient");
		case ERouteGpsCaptureFailureReason::InsufficientSamples: return TEXT("InsufficientSamples");
		case ERouteGpsCaptureFailureReason::UnstablePosition: return TEXT("UnstablePosition");
		case ERouteGpsCaptureFailureReason::SelectionInvalidated: return TEXT("SelectionInvalidated");
		case ERouteGpsCaptureFailureReason::Superseded: return TEXT("Superseded");
		default: return TEXT("Unknown");
		}
	}

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

	std::string WorkflowToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value);
		return std::string(Converted.Get(), Converted.Length());
	}

	bool WorkflowParseDigestHex(
		const FString& Hex,
		dspgcs::MavFtpRuntimeConfigDigest& OutDigest)
	{
		if (Hex.Len() != 64) return false;
		auto Nibble = [](const TCHAR Character, uint8& Out)
		{
			if (Character >= TEXT('0') && Character <= TEXT('9'))
				Out = static_cast<uint8>(Character - TEXT('0'));
			else if (Character >= TEXT('a') && Character <= TEXT('f'))
				Out = static_cast<uint8>(Character - TEXT('a') + 10);
			else return false;
			return true;
		};
		for (int32 Index = 0; Index < 32; ++Index)
		{
			uint8 High = 0, Low = 0;
			if (!Nibble(Hex[Index * 2], High) ||
				!Nibble(Hex[Index * 2 + 1], Low)) return false;
			OutDigest[Index] = static_cast<uint8>((High << 4) | Low);
		}
		return true;
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
			else if (Value.StartsWith(TEXT("CMB_"))) Type = UComboBoxString::StaticClass();
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

	bool SameStableAssignmentSource(
		const FRouteAssignmentSourceIdentity& Left,
		const FRouteAssignmentSourceIdentity& Right)
	{
		return Left.SourceDatasetDigest == Right.SourceDatasetDigest
			&& Left.NormalizedShowOriginLatitudeE7 == Right.NormalizedShowOriginLatitudeE7
			&& Left.NormalizedShowOriginLongitudeE7 == Right.NormalizedShowOriginLongitudeE7
			&& Left.FleetSessionID == Right.FleetSessionID
			&& Left.RouteDatasetGeneration == Right.RouteDatasetGeneration
			&& Left.bShowOriginSet == Right.bShowOriginSet
			&& Left.Latitude == Right.Latitude
			&& Left.Longitude == Right.Longitude
			&& Left.Altitude == Right.Altitude;
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
	ClearConfirmedRouteSlotDisplayOverride();
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
	ConfirmedNpzMapping = FGCSConfirmedNpzMappingSnapshot{};
	SelectedShowOriginDroneID.Reset();
	ActiveShowOriginCaptureRequestID.Reset();
	ActiveShowOriginCaptureRevision = 0;
	LastIssuedShowOriginCandidateRevision = 0;
	ShowOriginCandidate = FGCSShowOriginCandidate{};
	ShowOriginDraftSource = EGCSShowOriginDraftSource::Official;
	bUpdatingShowOriginUi = false;
	bShowOriginDraftActive = false;
	bHasConfirmedNpzMapping = false;
	LastIssuedConfirmationRevision = 0;
#if WITH_DEV_AUTOMATION_TESTS
	WorkflowFactoryForTest = {};
	bSkipRequiredWidgetBindingForTest = false;
	CommandTestHooks = {};
	AutoStartTestHooks = {};
	CommandGateInputsForTest.Reset();
	CloseRequestCountForTest = 0;
	ApplyNpzNumberingClickCountForTest = 0;
	CaptureStableGpsClickCountForTest = 0;
	ClearNpzNumberingClickCountForTest = 0;
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
		TEXT("BTN_CaptureStableGps"),TEXT("BTN_ApplyNpzNumbering"),TEXT("BTN_ClearNpzNumbering"),
		TEXT("TXT_NpzGpsMappingStatusValue"),TEXT("TXT_NpzGpsMappingCounts"),
		TEXT("TXT_NpzGpsMappingDescription"),TEXT("TXT_NpzMappingExceptions"),TEXT("TXT_NpzMappingWarning"),
		TEXT("CMB_ShowOriginDroneId"),TEXT("BTN_RefreshShowOriginDrones"),
		TEXT("TXT_ShowOriginGpsStatus"),TEXT("BTN_CaptureShowOriginGps"),
		TEXT("BTN_ApplyShowOriginGps"),TEXT("TXT_ShowOriginGpsSource"),
		TEXT("TXT_ShowOriginCaptureTime"),
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
	SetEdit(UploadWidgets, TEXT("EDT_MaxConcurrency"),
		FString::FromInt(AppliedMaxConcurrentTargets));
	ApplyReadOnlyControlState();
	BindCommandControls();
	ApplyConfirmedRouteSlotDisplayOverride();
	bBindingValid = true;
	RefreshShowOriginDroneList();
	RefreshShowOriginCaptureState();
	return true;
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
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ApplyNpzNumbering")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyNpzNumberingClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyNpzNumberingClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_CaptureStableGps")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCaptureStableGpsClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCaptureStableGpsClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ClearNpzNumbering")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleClearNpzNumberingClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleClearNpzNumberingClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_RefreshShowOriginDrones")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRefreshShowOriginDronesClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRefreshShowOriginDronesClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_CaptureShowOriginGps")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCaptureShowOriginGpsClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCaptureShowOriginGpsClicked);
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ApplyShowOriginGps")))
	{
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyShowOriginGpsClicked);
		Button->OnClicked.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyShowOriginGpsClicked);
	}
	if (UComboBoxString* Combo = Get<UComboBoxString>(ConvertWidgets, TEXT("CMB_ShowOriginDroneId")))
	{
		Combo->OnSelectionChanged.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleShowOriginDroneSelectionChanged);
		Combo->OnSelectionChanged.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleShowOriginDroneSelectionChanged);
	}
	for (const FName Name : {FName(TEXT("EDT_OriginLatitude")),
		FName(TEXT("EDT_OriginLongitude")), FName(TEXT("EDT_OriginAltitude"))})
	{
		if (UEditableTextBox* Box = Get<UEditableTextBox>(ConvertWidgets, Name))
		{
			Box->OnTextChanged.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleShowOriginFieldTextChanged);
			Box->OnTextChanged.AddDynamic(this, &UGCSRouteWorkflowCoordinator::HandleShowOriginFieldTextChanged);
		}
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
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ApplyNpzNumbering")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyNpzNumberingClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_CaptureStableGps")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCaptureStableGpsClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ClearNpzNumbering")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleClearNpzNumberingClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_RefreshShowOriginDrones")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRefreshShowOriginDronesClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_CaptureShowOriginGps")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleCaptureShowOriginGpsClicked);
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ApplyShowOriginGps")))
		Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleApplyShowOriginGpsClicked);
	if (UComboBoxString* Combo = Get<UComboBoxString>(ConvertWidgets, TEXT("CMB_ShowOriginDroneId")))
		Combo->OnSelectionChanged.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleShowOriginDroneSelectionChanged);
	for (const FName Name : {FName(TEXT("EDT_OriginLatitude")),
		FName(TEXT("EDT_OriginLongitude")), FName(TEXT("EDT_OriginAltitude"))})
		if (UEditableTextBox* Box = Get<UEditableTextBox>(ConvertWidgets, Name))
			Box->OnTextChanged.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleShowOriginFieldTextChanged);
}

bool UGCSRouteWorkflowCoordinator::BuildCaptureBoundAssignmentSource(
	const FRouteAssignmentSourceSnapshot& RouteAndFleetSource,
	const FRouteGpsCaptureSnapshot& Capture,
	FRouteAssignmentSourceSnapshot& OutSource,
	FString& OutError)
{
	OutSource = FRouteAssignmentSourceSnapshot{};
	OutError.Reset();
	if (Capture.State != ERouteGpsCaptureState::Ready)
	{
		OutError = TEXT("F.1 GPS Capture is not Ready.");
		return false;
	}
	if (Capture.FleetSessionID.IsEmpty() ||
		Capture.FleetSessionID != RouteAndFleetSource.SourceIdentity.FleetSessionID)
	{
		OutError = TEXT("F.1 GPS Capture Fleet session does not match the Route Assignment source.");
		return false;
	}
	if (Capture.SelectedDroneIDs.IsEmpty() ||
		Capture.Participants.Num() != Capture.SelectedDroneIDs.Num())
	{
		OutError = TEXT("F.1 GPS Capture participant set is incomplete.");
		return false;
	}

	OutSource.SourceIdentity = RouteAndFleetSource.SourceIdentity;
	OutSource.RouteSlots = RouteAndFleetSource.RouteSlots;
	OutSource.Drones.Reserve(Capture.Participants.Num());
	TSet<FString> Selected(Capture.SelectedDroneIDs);
	TSet<FString> Seen;
	for (const FRouteGpsCaptureParticipantResult& Participant : Capture.Participants)
	{
		if (!Selected.Contains(Participant.DroneID) || Seen.Contains(Participant.DroneID) ||
			Participant.EndpointIdentity.TrimStartAndEnd().IsEmpty() ||
			Participant.Status != ERouteGpsCaptureParticipantStatus::Ready ||
			!FMath::IsFinite(Participant.EstimatedLatitude) ||
			!FMath::IsFinite(Participant.EstimatedLongitude))
		{
			OutError = FString::Printf(
				TEXT("F.1 GPS Capture participant is invalid or duplicated: %s"),
				*Participant.DroneID);
			OutSource = FRouteAssignmentSourceSnapshot{};
			return false;
		}
		Seen.Add(Participant.DroneID);
		FRouteAssignmentDroneSource Drone;
		Drone.DroneID = Participant.DroneID;
		Drone.Latitude = Participant.EstimatedLatitude;
		Drone.Longitude = Participant.EstimatedLongitude;
		Drone.AltitudeAMSL = Participant.AltitudeAMSL;
		Drone.bGPSValid = true;
		Drone.GPSFixType = Participant.FixType;
		Drone.bAltitudeValid = true;
		Drone.bOnline = true;
		OutSource.Drones.Add(MoveTemp(Drone));
	}
	OutSource.Drones.Sort([](const FRouteAssignmentDroneSource& Left,
		const FRouteAssignmentDroneSource& Right)
	{
		return Left.DroneID < Right.DroneID;
	});
	return Seen.Num() == Capture.SelectedDroneIDs.Num();
}

bool UGCSRouteWorkflowCoordinator::IsConfirmedMappingMutationBlocked(FString& OutError) const
{
	OutError.Reset();
	if (IsCommandSelectionMutationBlocked())
	{
		OutError = TEXT("Cannot change NPZ numbering while an upload transaction or Job is active.");
		return true;
	}
	if (GameInstanceRef.IsValid())
	{
		const UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
			GameInstanceRef->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
		if (Sync && Sync->GetSyncState() == EDSPGCSFrozenAssignmentSyncState::SyncingToConsole)
		{
			OutError = TEXT("Cannot change NPZ numbering while Frozen Assignment sync is active.");
			return true;
		}
	}
	return false;
}

bool UGCSRouteWorkflowCoordinator::BuildConfirmedNpzMapping(
	FGCSConfirmedNpzMappingSnapshot& OutMapping,
	FString& OutError)
{
	OutMapping = FGCSConfirmedNpzMappingSnapshot{};
	OutError.Reset();
	if (!GameInstanceRef.IsValid())
	{
		OutError = TEXT("Route Workflow Coordinator is not initialized.");
		return false;
	}
	UGameInstance* GI = GameInstanceRef.Get();
	UGCSUIBridgeSubsystem* Bridge = GI->GetSubsystem<UGCSUIBridgeSubsystem>();
	UFleetManagerSubsystem* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
	URouteGpsCaptureSubsystem* CaptureAuthority = GI->GetSubsystem<URouteGpsCaptureSubsystem>();
	URouteAssignmentSubsystem* Assignment = GI->GetSubsystem<URouteAssignmentSubsystem>();
	if (!Bridge || !Fleet || !Assignment)
	{
		OutError = TEXT("Selection, Fleet, or Route Assignment authority is unavailable.");
		return false;
	}

	TArray<FString> Selection = Bridge->GetSelectedCommandDroneIDs();
	Selection.Sort();
	if (Selection.IsEmpty())
	{
		OutError = TEXT("NPZ numbering requires at least one selected drone.");
		return false;
	}

	FRouteAssignmentSourceSnapshot FullSource;
	if (!Assignment->CaptureSourceSnapshot(FullSource, OutError))
	{
		return false;
	}
	if (FullSource.SourceIdentity.FleetSessionID.IsEmpty() ||
		FullSource.SourceIdentity.FleetSessionID != Fleet->GetActiveFleetSessionID())
	{
		OutError = TEXT("Route Assignment and live Fleet session identities do not match.");
		return false;
	}

	FRouteGpsCaptureSnapshot Capture;
	if (!CaptureAuthority || !CaptureAuthority->GetLatestCommandSelectionSnapshot(Capture) ||
		Capture.State != ERouteGpsCaptureState::Ready)
	{
		OutError = TEXT("F.1 GPS Capture must be Ready before confirming NPZ numbering.");
		return false;
	}
	if (Capture.SelectedDroneIDs != Selection)
	{
		OutError = TEXT("F.1 GPS Capture Selection no longer matches the current Command Selection.");
		return false;
	}
	FRouteAssignmentSourceSnapshot NumberingSource;
	TMap<FString, FString> EndpointByDroneID;
	if (!BuildCaptureBoundAssignmentSource(FullSource, Capture, NumberingSource, OutError))
	{
		return false;
	}
	for (const FRouteGpsCaptureParticipantResult& Participant : Capture.Participants)
	{
		EndpointByDroneID.Add(Participant.DroneID, Participant.EndpointIdentity);
	}

	for (const FString& DroneID : Selection)
	{
		FDroneData CurrentDrone;
		if (!Fleet->GetDroneByID(DroneID, CurrentDrone) || !CurrentDrone.bOnline ||
			CurrentDrone.EndpointIdentity.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Selected NPZ numbering participant identity is unavailable: %s"),
				*DroneID);
			return false;
		}
		const FString* CapturedEndpoint = EndpointByDroneID.Find(DroneID);
		if (!CapturedEndpoint || *CapturedEndpoint != CurrentDrone.EndpointIdentity)
		{
			OutError = FString::Printf(
				TEXT("F.1 participant endpoint is no longer current: %s"), *DroneID);
			return false;
		}
		EndpointByDroneID.Add(DroneID, CurrentDrone.EndpointIdentity);
	}

	FRouteAssignmentCandidate Candidate;
	if (!RouteAssignmentPipeline::BuildCandidateFromSourceSnapshot(
		NumberingSource, Candidate, OutError))
	{
		return false;
	}
	if (LastIssuedConfirmationRevision == MAX_int64)
	{
		OutError = TEXT("Confirmed NPZ Mapping revision space is exhausted.");
		return false;
	}
	FGCSConfirmedNpzMappingSnapshot Mapping;
	Mapping.ConfirmationRevision = LastIssuedConfirmationRevision + 1;
	Mapping.RouteDatasetGeneration = NumberingSource.SourceIdentity.RouteDatasetGeneration;
	Mapping.SourceDatasetDigest = NumberingSource.SourceIdentity.SourceDatasetDigest.ToLowerHex();
	Mapping.RouteCount = NumberingSource.RouteSlots.Num();
	Mapping.NormalizedShowOriginLatitudeE7 =
		NumberingSource.SourceIdentity.NormalizedShowOriginLatitudeE7;
	Mapping.NormalizedShowOriginLongitudeE7 =
		NumberingSource.SourceIdentity.NormalizedShowOriginLongitudeE7;
	Mapping.F1CaptureAuthoritySessionID = Capture.CaptureAuthoritySessionID;
	Mapping.F1CaptureRequestID = Capture.CaptureRequestID;
	Mapping.F1CaptureRevision = Capture.CaptureRevision;
	Mapping.SelectionDigest = Capture.SelectionDigest;
	Mapping.SelectedDroneIDs = Selection;
	Mapping.FleetSessionID = NumberingSource.SourceIdentity.FleetSessionID;
	Mapping.Entries.Reserve(Candidate.MatchedEntries.Num());
	TSet<FString> MatchedDroneIDs;
	for (const FRouteAssignmentEntry& Match : Candidate.MatchedEntries)
	{
		const FString* Endpoint = EndpointByDroneID.Find(Match.DroneID);
		if (!Endpoint)
		{
			OutError = TEXT("Matcher produced a DroneID outside the selected participant set.");
			return false;
		}
		FGCSConfirmedNpzMappingEntry Entry;
		Entry.DroneID = Match.DroneID;
		Entry.EndpointIdentity = *Endpoint;
		Entry.RouteSlotIndex = Match.RouteSlotIndex;
		Entry.MatchDistanceMeters = Match.HorizontalDistanceMeters;
		Mapping.Entries.Add(MoveTemp(Entry));
		MatchedDroneIDs.Add(Match.DroneID);
	}
	for (const FString& DroneID : Selection)
	{
		if (!MatchedDroneIDs.Contains(DroneID))
		{
			Mapping.UnassignedDroneIDs.Add(DroneID);
		}
	}
	for (const FRouteAssignmentSpareDrone& Spare : Candidate.SpareDrones)
	{
		Mapping.SpareDroneIDs.Add(Spare.DroneID);
	}
	Mapping.UnassignedDroneIDs.Sort();
	Mapping.SpareDroneIDs.Sort();
	Mapping.VacantRouteSlots = Candidate.VacantRouteSlots;
	Mapping.VacantRouteSlots.Sort();
	Mapping.Entries.Sort([](const FGCSConfirmedNpzMappingEntry& Left,
		const FGCSConfirmedNpzMappingEntry& Right)
	{
		return Left.DroneID < Right.DroneID;
	});
	Mapping.bValid = true;

	Assignment->Reset();
	if (!Assignment->ReplaceWorkingAssignment(Candidate, OutError))
	{
		return false;
	}
	LastIssuedConfirmationRevision = Mapping.ConfirmationRevision;
	OutMapping = MoveTemp(Mapping);
	return true;
}

bool UGCSRouteWorkflowCoordinator::ValidateConfirmedNpzMappingCurrent(
	FString& OutError) const
{
	OutError.Reset();
	if (!bHasConfirmedNpzMapping)
	{
		OutError = TEXT("No Confirmed NPZ Mapping exists.");
		return false;
	}
	if (!ConfirmedNpzMapping.bValid)
	{
		OutError = ConfirmedNpzMapping.InvalidReason.IsEmpty()
			? TEXT("Confirmed NPZ Mapping is invalidated; Clear or confirm again.")
			: ConfirmedNpzMapping.InvalidReason;
		return false;
	}
	if (!GameInstanceRef.IsValid())
	{
		OutError = TEXT("Route Workflow Coordinator is not initialized.");
		return false;
	}
	const UGameInstance* GI = GameInstanceRef.Get();
	const UFleetManagerSubsystem* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
	const URouteAssignmentSubsystem* Assignment = GI->GetSubsystem<URouteAssignmentSubsystem>();
	if (!Fleet || !Assignment)
	{
		OutError = TEXT("Confirmed NPZ Mapping authority dependencies are unavailable.");
		return false;
	}
	// Confirmation freezes its own participant set and F.1 capture provenance for
	// the lifetime of this UE session. Command selection is only an operator input
	// after that point; selection changes and the selection-bound F.1 snapshot must
	// not erase an already confirmed DroneID -> RouteSlot mapping.
	if (Fleet->GetActiveFleetSessionID() != ConfirmedNpzMapping.FleetSessionID)
	{
		OutError = TEXT("Confirmed NPZ Mapping was invalidated by a Fleet session change.");
		return false;
	}
	FRouteAssignmentSourceSnapshot Source;
	if (!Assignment->CaptureSourceSnapshot(Source, OutError))
	{
		return false;
	}
	if (Source.SourceIdentity.RouteDatasetGeneration != ConfirmedNpzMapping.RouteDatasetGeneration ||
		Source.SourceIdentity.SourceDatasetDigest.ToLowerHex() != ConfirmedNpzMapping.SourceDatasetDigest ||
		Source.RouteSlots.Num() != ConfirmedNpzMapping.RouteCount ||
		Source.SourceIdentity.NormalizedShowOriginLatitudeE7 !=
			ConfirmedNpzMapping.NormalizedShowOriginLatitudeE7 ||
		Source.SourceIdentity.NormalizedShowOriginLongitudeE7 !=
			ConfirmedNpzMapping.NormalizedShowOriginLongitudeE7)
	{
		OutError = TEXT("Confirmed NPZ Mapping Route dataset or Show Origin changed.");
		return false;
	}
	TSet<FString> DroneIDs;
	TSet<int32> RouteSlots;
	for (const FGCSConfirmedNpzMappingEntry& Entry : ConfirmedNpzMapping.Entries)
	{
		FDroneData Drone;
		if (Entry.DroneID.IsEmpty() || DroneIDs.Contains(Entry.DroneID) ||
			Entry.RouteSlotIndex < 0 || Entry.RouteSlotIndex >= ConfirmedNpzMapping.RouteCount ||
			RouteSlots.Contains(Entry.RouteSlotIndex) ||
			!Fleet->GetDroneByID(Entry.DroneID, Drone) || !Drone.bOnline ||
			Drone.EndpointIdentity != Entry.EndpointIdentity)
		{
			OutError = FString::Printf(
				TEXT("Confirmed NPZ Mapping participant or endpoint changed: %s"),
				*Entry.DroneID);
			return false;
		}
		DroneIDs.Add(Entry.DroneID);
		RouteSlots.Add(Entry.RouteSlotIndex);
	}
	return true;
}

void UGCSRouteWorkflowCoordinator::ApplyConfirmedRouteSlotDisplayOverride() const
{
	if (!GameInstanceRef.IsValid()) return;
	UWorld* World = GameInstanceRef->GetWorld();
	if (!World) return;
	TMap<FString, int32> Overrides;
	if (bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid)
	{
		for (const FGCSConfirmedNpzMappingEntry& Entry : ConfirmedNpzMapping.Entries)
		{
			Overrides.Add(Entry.DroneID, Entry.RouteSlotIndex);
		}
	}
	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		It->SetConfirmedRouteSlotDisplayOverrides(Overrides);
		It->SetNpzNumberingAnomalyDroneIDs(
			bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid
				? ConfirmedNpzMapping.UnassignedDroneIDs : TArray<FString>{});
	}
}

void UGCSRouteWorkflowCoordinator::ClearConfirmedRouteSlotDisplayOverride() const
{
	if (!GameInstanceRef.IsValid()) return;
	UWorld* World = GameInstanceRef->GetWorld();
	if (!World) return;
	for (TActorIterator<ADroneFleetVisualizationActor> It(World); It; ++It)
	{
		It->ClearConfirmedRouteSlotDisplayOverrides();
		It->ClearNpzNumberingAnomalyDroneIDs();
	}
}

void UGCSRouteWorkflowCoordinator::InvalidateConfirmedNpzMapping(
	const FString& Reason,
	const bool bResetWorkingAssignment)
{
	if (!bHasConfirmedNpzMapping || !ConfirmedNpzMapping.bValid) return;
	ConfirmedNpzMapping.bValid = false;
	ConfirmedNpzMapping.InvalidReason = Reason.IsEmpty()
		? TEXT("Confirmed NPZ Mapping is invalidated; Clear or confirm again.")
		: Reason;
	ClearConfirmedRouteSlotDisplayOverride();
	if (bResetWorkingAssignment && GameInstanceRef.IsValid())
	{
		if (URouteAssignmentSubsystem* Assignment =
			GameInstanceRef->GetSubsystem<URouteAssignmentSubsystem>())
		{
			Assignment->Reset();
		}
	}
}

void UGCSRouteWorkflowCoordinator::ClearConfirmedNpzMapping(
	const bool bResetWorkingAssignment)
{
	ClearConfirmedRouteSlotDisplayOverride();
	ConfirmedNpzMapping = FGCSConfirmedNpzMappingSnapshot{};
	bHasConfirmedNpzMapping = false;
	if (bResetWorkingAssignment && GameInstanceRef.IsValid())
	{
		if (URouteAssignmentSubsystem* Assignment =
			GameInstanceRef->GetSubsystem<URouteAssignmentSubsystem>())
		{
			Assignment->Reset();
		}
	}
}

void UGCSRouteWorkflowCoordinator::HandleCaptureStableGpsClicked()
{
	if (!IsInGameThread()) return;
#if WITH_DEV_AUTOMATION_TESTS
	++CaptureStableGpsClickCountForTest;
#endif
	FString Error;
	if (IsConfirmedMappingMutationBlocked(Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview, Error);
		return;
	}
	if (!GameInstanceRef.IsValid())
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview,
			TEXT("Route Workflow Coordinator is not initialized."));
		return;
	}
	FRouteGpsCapturePolicy Policy;
	if (!URouteGpsCaptureSubsystem::LoadConfiguredPolicy(Policy, Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview, Error);
		return;
	}
#if UE_BUILD_SHIPPING
	if (!Policy.bHardwareValidated)
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview,
			TEXT("GPS Capture policy is not hardware-validated; Shipping capture is fail-closed."));
		return;
	}
#endif
	URouteGpsCaptureSubsystem* CaptureAuthority =
		GameInstanceRef->GetSubsystem<URouteGpsCaptureSubsystem>();
	if (!CaptureAuthority || !CaptureAuthority->BeginCapture(Policy, Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview,
			Error.IsEmpty() ? TEXT("Stable GPS Capture could not start.") : Error);
		return;
	}

	// A newly accepted capture is a new identity-bound transaction. Only now is
	// the previously confirmed mapping invalidated; a rejected click preserves it.
	ClearConfirmedNpzMapping(true);
	ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview);
	FRouteGpsCaptureSnapshot Snapshot;
	CaptureAuthority->GetCurrentSnapshot(Snapshot);
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[F1_GPS_CAPTURE] Started Profile=%s HardwareValidated=%s Request=%s Revision=%lld Selected=%d"),
		*Policy.ProfileName, Policy.bHardwareValidated ? TEXT("yes") : TEXT("no"),
		*Snapshot.CaptureRequestID, Snapshot.CaptureRevision,
		Snapshot.SelectedDroneIDs.Num());
	RefreshReadOnlyState();
}

void UGCSRouteWorkflowCoordinator::HandleApplyNpzNumberingClicked()
{
	if (!IsInGameThread()) return;
#if WITH_DEV_AUTOMATION_TESTS
	++ApplyNpzNumberingClickCountForTest;
#endif
	FString Error;
	if (IsConfirmedMappingMutationBlocked(Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview, Error);
		return;
	}
	FGCSConfirmedNpzMappingSnapshot Mapping;
	if (!BuildConfirmedNpzMapping(Mapping, Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview,
			Error.IsEmpty() ? TEXT("NPZ numbering confirmation was rejected.") : Error);
		return;
	}
	ConfirmedNpzMapping = MoveTemp(Mapping);
	bHasConfirmedNpzMapping = true;
	ApplyConfirmedRouteSlotDisplayOverride();
	ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview);
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[NPZ_NUMBERING] Confirmed Revision=%lld Authority=StableF1Capture Frame=FixedShowOriginENU Snapshot=%s Matched=%d Abnormal=%d Vacant=%d Spare=%d RouteSlots=%d"),
		ConfirmedNpzMapping.ConfirmationRevision,
		*ConfirmedNpzMapping.F1CaptureRequestID,
		ConfirmedNpzMapping.Entries.Num(), ConfirmedNpzMapping.UnassignedDroneIDs.Num(),
		ConfirmedNpzMapping.VacantRouteSlots.Num(), ConfirmedNpzMapping.SpareDroneIDs.Num(),
		ConfirmedNpzMapping.RouteCount);
	RefreshReadOnlyState();
}

void UGCSRouteWorkflowCoordinator::HandleClearNpzNumberingClicked()
{
	if (!IsInGameThread()) return;
#if WITH_DEV_AUTOMATION_TESTS
	++ClearNpzNumberingClickCountForTest;
#endif
	FString Error;
	if (IsConfirmedMappingMutationBlocked(Error))
	{
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview, Error);
		return;
	}
	ClearConfirmedNpzMapping(true);
	ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview);
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[NPZ_NUMBERING] Cleared; default NumericDroneID RouteSlot mapping is available for F.1.5."));
	RefreshReadOnlyState();
}

void UGCSRouteWorkflowCoordinator::HandleRefreshShowOriginDronesClicked()
{
	if (!IsInGameThread()) return;
	RefreshShowOriginDroneList();
	RefreshShowOriginCaptureState();
	UpdateCommandControlState();
}

void UGCSRouteWorkflowCoordinator::RefreshShowOriginDroneList()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!GameInstanceRef.IsValid()) return;
	UComboBoxString* Combo = Get<UComboBoxString>(
		ConvertWidgets, TEXT("CMB_ShowOriginDroneId"));
	const UFleetManagerSubsystem* Fleet =
		GameInstanceRef->GetSubsystem<UFleetManagerSubsystem>();
	if (!Combo || !Fleet) return;

	TArray<FString> OnlineDroneIDs;
	for (const FDroneData& Drone : Fleet->GetDronePage(0, Fleet->GetDroneCount()))
	{
		if (Drone.bOnline && !Drone.DroneID.TrimStartAndEnd().IsEmpty())
		{
			OnlineDroneIDs.Add(Drone.DroneID);
		}
	}
	OnlineDroneIDs.Sort();
	const bool bPreserveSelection =
		!SelectedShowOriginDroneID.IsEmpty()
		&& OnlineDroneIDs.Contains(SelectedShowOriginDroneID);

	TGuardValue<bool> Guard(bUpdatingShowOriginUi, true);
	Combo->ClearOptions();
	for (const FString& DroneID : OnlineDroneIDs)
	{
		Combo->AddOption(DroneID);
	}
	if (bPreserveSelection)
	{
		Combo->SetSelectedOption(SelectedShowOriginDroneID);
	}
	else
	{
		const bool bWasSelected = !SelectedShowOriginDroneID.IsEmpty();
		SelectedShowOriginDroneID.Reset();
		Combo->ClearSelection();
		if (bWasSelected)
		{
			InvalidateShowOriginCandidate(
				TEXT("所选无人机已离线，请重新选择"));
			SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
				TEXT("所选无人机已离线，请重新选择"));
		}
		else if (OnlineDroneIDs.IsEmpty())
		{
			SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
				TEXT("当前没有在线无人机"));
		}
	}
}

void UGCSRouteWorkflowCoordinator::HandleShowOriginDroneSelectionChanged(
	FString SelectedItem,
	ESelectInfo::Type SelectionType)
{
	(void)SelectionType;
	if (!IsInGameThread() || bUpdatingShowOriginUi) return;
	SelectedItem = SelectedItem.TrimStartAndEnd();
	if (SelectedItem == SelectedShowOriginDroneID) return;
	if (ShowOriginCandidate.bValid && !ShowOriginCandidate.bApplied)
	{
		InvalidateShowOriginCandidate(
			TEXT("已切换表演原点来源无人机；旧GPS候选已失效"));
	}
	SelectedShowOriginDroneID = MoveTemp(SelectedItem);
	ActiveShowOriginCaptureRequestID.Reset();
	ActiveShowOriginCaptureRevision = 0;
	RefreshShowOriginCaptureState();
	UpdateCommandControlState();
}

void UGCSRouteWorkflowCoordinator::HandleCaptureShowOriginGpsClicked()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	if (SelectedShowOriginDroneID.IsEmpty())
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
			TEXT("请先选择一架在线无人机"));
		return;
	}

	FString Error;
	FRouteGpsCapturePolicy Policy;
	if (!URouteGpsCaptureSubsystem::LoadConfiguredPolicy(Policy, Error))
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), Error);
		return;
	}
#if UE_BUILD_SHIPPING
	if (!Policy.bHardwareValidated)
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
			TEXT("GPS采集策略尚未通过硬件验证，Shipping模式拒绝采集"));
		return;
	}
#endif
	URouteGpsCaptureSubsystem* CaptureAuthority =
		GameInstanceRef->GetSubsystem<URouteGpsCaptureSubsystem>();
	const TArray<FString> Participants{SelectedShowOriginDroneID};
	if (!CaptureAuthority ||
		!CaptureAuthority->BeginCaptureForParticipants(Policy, Participants, Error))
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
			Error.IsEmpty() ? TEXT("稳定GPS采集无法启动") : Error);
		return;
	}

	FRouteGpsCaptureSnapshot Snapshot;
	CaptureAuthority->GetCurrentSnapshot(Snapshot);
	if (ShowOriginCandidate.bValid && !ShowOriginCandidate.bApplied)
	{
		InvalidateShowOriginCandidate(TEXT("新的GPS采集已替换旧候选"));
	}
	ShowOriginCandidate = FGCSShowOriginCandidate{};
	ActiveShowOriginCaptureRequestID = Snapshot.CaptureRequestID;
	ActiveShowOriginCaptureRevision = Snapshot.CaptureRevision;
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
		FString::Printf(TEXT("%s｜正在采集稳定GPS"), *SelectedShowOriginDroneID));
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsSource"),
		FString::Printf(TEXT("来源：%s（采集中）"), *SelectedShowOriginDroneID));
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginCaptureTime"), TEXT("采集时间：--"));
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[SHOW_ORIGIN_CAPTURE] Started Drone=%s EndpointBound=yes Request=%s Revision=%lld Policy=%s"),
		*SelectedShowOriginDroneID, *Snapshot.CaptureRequestID,
		Snapshot.CaptureRevision, *Policy.ProfileName);
	UpdateCommandControlState();
}

void UGCSRouteWorkflowCoordinator::InvalidateShowOriginCandidate(
	const FString& Reason)
{
	ActiveShowOriginCaptureRequestID.Reset();
	ActiveShowOriginCaptureRevision = 0;
	if (ShowOriginCandidate.bApplied)
	{
		ShowOriginCandidate = FGCSShowOriginCandidate{};
		ShowOriginDraftSource = EGCSShowOriginDraftSource::Official;
		return;
	}
	ShowOriginCandidate.bValid = false;
	ShowOriginCandidate.bGpsProvenanceIntact = false;
	ShowOriginCandidate.InvalidReason = Reason;
	if (bShowOriginDraftActive)
	{
		ShowOriginDraftSource = EGCSShowOriginDraftSource::InvalidatedCandidate;
	}
}

void UGCSRouteWorkflowCoordinator::RefreshShowOriginCaptureState()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!GameInstanceRef.IsValid()) return;
	const UFleetManagerSubsystem* Fleet =
		GameInstanceRef->GetSubsystem<UFleetManagerSubsystem>();
	if (SelectedShowOriginDroneID.IsEmpty()) return;

	if (ActiveShowOriginCaptureRequestID.IsEmpty())
	{
		if (ShowOriginDraftSource == EGCSShowOriginDraftSource::CapturedCandidate
			|| ShowOriginDraftSource == EGCSShowOriginDraftSource::Manual
			|| ShowOriginDraftSource == EGCSShowOriginDraftSource::InvalidatedCandidate)
		{
			return;
		}
		FDroneData Drone;
		if (!Fleet || !Fleet->GetDroneByID(SelectedShowOriginDroneID, Drone)
			|| !Drone.bOnline)
		{
			SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
				TEXT("所选无人机已离线，请重新选择"));
			return;
		}
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), FString::Printf(
			TEXT("%s｜Online｜Fix %d｜GPS %s"), *Drone.DroneID,
			Drone.GPSFixType, DroneGPS::IsValid(Drone) ? TEXT("有效") : TEXT("不可用")));
		return;
	}

	const URouteGpsCaptureSubsystem* CaptureAuthority =
		GameInstanceRef->GetSubsystem<URouteGpsCaptureSubsystem>();
	FRouteGpsCaptureSnapshot Snapshot;
	if (!CaptureAuthority || !CaptureAuthority->GetCurrentSnapshot(Snapshot)
		|| Snapshot.CaptureRequestID != ActiveShowOriginCaptureRequestID
		|| Snapshot.CaptureRevision != ActiveShowOriginCaptureRevision)
	{
		InvalidateShowOriginCandidate(TEXT("GPS候选已被新的Capture事务替换"));
		ShowOriginDraftSource = EGCSShowOriginDraftSource::InvalidatedCandidate;
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
			TEXT("GPS候选已失效：Capture已被替换"));
		ActiveShowOriginCaptureRequestID.Reset();
		ActiveShowOriginCaptureRevision = 0;
		return;
	}

	if (Snapshot.State == ERouteGpsCaptureState::Capturing)
	{
		const int32 Samples = Snapshot.Participants.IsEmpty()
			? 0 : Snapshot.Participants[0].UniqueSampleCount;
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), FString::Printf(
			TEXT("%s｜正在采集稳定GPS｜样本 %d/%d"),
			*SelectedShowOriginDroneID, Samples, Snapshot.Policy.MinUniqueSamples));
		return;
	}
	if (Snapshot.State == ERouteGpsCaptureState::Failed
		|| Snapshot.State == ERouteGpsCaptureState::Invalidated)
	{
		const ERouteGpsCaptureFailureReason Failure = Snapshot.Participants.IsEmpty()
			? ERouteGpsCaptureFailureReason::GpsUnavailable
			: Snapshot.Participants[0].FailureReason;
		InvalidateShowOriginCandidate(FString::Printf(
			TEXT("GPS采集失败：%s"), *GpsCaptureFailure(Failure)));
		ShowOriginDraftSource = EGCSShowOriginDraftSource::InvalidatedCandidate;
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), FString::Printf(
			TEXT("%s｜GPS采集失败：%s"), *SelectedShowOriginDroneID,
			*GpsCaptureFailure(Failure)));
		return;
	}
	if (Snapshot.State != ERouteGpsCaptureState::Ready
		|| Snapshot.Participants.Num() != 1) return;
	if (ShowOriginCandidate.bValid
		&& ShowOriginCandidate.CaptureRequestID == Snapshot.CaptureRequestID
		&& ShowOriginCandidate.CaptureRevision == Snapshot.CaptureRevision)
	{
		return;
	}

	const FRouteGpsCaptureParticipantResult& Participant = Snapshot.Participants[0];
	if (Participant.Status != ERouteGpsCaptureParticipantStatus::Ready
		|| Participant.DroneID != SelectedShowOriginDroneID)
	{
		InvalidateShowOriginCandidate(TEXT("Ready Capture参与者与当前Anchor不一致"));
		ShowOriginDraftSource = EGCSShowOriginDraftSource::InvalidatedCandidate;
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
			TEXT("GPS候选已失效：参与者身份不一致"));
		return;
	}

	FGCSShowOriginCandidate Candidate;
	Candidate.DroneID = Participant.DroneID;
	Candidate.EndpointIdentity = Participant.EndpointIdentity;
	Candidate.CaptureAuthoritySessionID = Snapshot.CaptureAuthoritySessionID;
	Candidate.CaptureRequestID = Snapshot.CaptureRequestID;
	Candidate.CaptureRevision = Snapshot.CaptureRevision;
	Candidate.Latitude = Participant.EstimatedLatitude;
	Candidate.Longitude = Participant.EstimatedLongitude;
	Candidate.AltitudeAMSL = Participant.AltitudeAMSL;
	Candidate.FixType = Participant.FixType;
	Candidate.FirstGpsSampleUtcMs = Participant.FirstGpsSampleUtcMs;
	Candidate.LastGpsSampleUtcMs = Participant.LastGpsSampleUtcMs;
	Candidate.UniqueSampleCount = Participant.UniqueSampleCount;
	Candidate.MedianResidualMeters = Participant.MedianResidualMeters;
	Candidate.RobustSpreadMeters = Participant.RobustSpreadMeters;
	Candidate.MaxResidualMeters = Participant.MaxResidualMeters;
	Candidate.CaptureCompletedUtcMs = Snapshot.CompletedAtUtc.ToUnixTimestamp() * 1000
		+ Snapshot.CompletedAtUtc.GetMillisecond();
	Candidate.FleetSessionID = Snapshot.FleetSessionID;
	Candidate.CandidateRevision = ++LastIssuedShowOriginCandidateRevision;
	Candidate.bValid = true;
	Candidate.bGpsProvenanceIntact = true;
	ShowOriginCandidate = MoveTemp(Candidate);
	ShowOriginDraftSource = EGCSShowOriginDraftSource::CapturedCandidate;
	bShowOriginDraftActive = true;
	SetShowOriginDraftFields(ShowOriginCandidate.Latitude,
		ShowOriginCandidate.Longitude, ShowOriginCandidate.AltitudeAMSL);
	const FDateTime NowUtc = FDateTime::UtcNow();
	const int64 NowMs = NowUtc.ToUnixTimestamp() * 1000 + NowUtc.GetMillisecond();
	const int64 AgeMs = FMath::Max<int64>(0, NowMs - ShowOriginCandidate.LastGpsSampleUtcMs);
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), FString::Printf(
		TEXT("%s｜GPS稳定｜%d/%d｜Age %lld ms"),
		*ShowOriginCandidate.DroneID, ShowOriginCandidate.UniqueSampleCount,
		Snapshot.Policy.MinUniqueSamples, AgeMs));
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsSource"), FString::Printf(
		TEXT("来源：%s（待应用）"), *ShowOriginCandidate.DroneID));
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginCaptureTime"), FString::Printf(
		TEXT("采集时间：%s"), *Snapshot.CompletedAtUtc.ToString(TEXT("%Y-%m-%d %H:%M:%S"))));
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[SHOW_ORIGIN_CAPTURE] Ready Drone=%s Request=%s CaptureRevision=%lld CandidateRevision=%lld Samples=%d Fix=%d Lat=%.9f Lon=%.9f AMSL=%.3f"),
		*ShowOriginCandidate.DroneID, *ShowOriginCandidate.CaptureRequestID,
		ShowOriginCandidate.CaptureRevision, ShowOriginCandidate.CandidateRevision,
		ShowOriginCandidate.UniqueSampleCount, ShowOriginCandidate.FixType,
		ShowOriginCandidate.Latitude, ShowOriginCandidate.Longitude,
		ShowOriginCandidate.AltitudeAMSL);
	UpdateCommandControlState();
}

void UGCSRouteWorkflowCoordinator::SetShowOriginDraftFields(
	const double Latitude,
	const double Longitude,
	const double AltitudeAMSL)
{
	using namespace GCSRouteWorkflowPrivate;
	TGuardValue<bool> Guard(bUpdatingShowOriginUi, true);
	SetEdit(ConvertWidgets, TEXT("EDT_OriginLatitude"),
		FString::Printf(TEXT("%.8f"), Latitude));
	SetEdit(ConvertWidgets, TEXT("EDT_OriginLongitude"),
		FString::Printf(TEXT("%.8f"), Longitude));
	SetEdit(ConvertWidgets, TEXT("EDT_OriginAltitude"),
		FString::Printf(TEXT("%.3f"), AltitudeAMSL));
}

void UGCSRouteWorkflowCoordinator::HandleShowOriginFieldTextChanged(const FText& Text)
{
	(void)Text;
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || bUpdatingShowOriginUi) return;
	bShowOriginDraftActive = true;
	if (ShowOriginCandidate.bValid && ShowOriginCandidate.bGpsProvenanceIntact)
	{
		ShowOriginCandidate.bGpsProvenanceIntact = false;
		ShowOriginCandidate.bValid = false;
		ShowOriginCandidate.InvalidReason = TEXT("候选坐标已手工修改");
	}
	ShowOriginDraftSource = EGCSShowOriginDraftSource::Manual;
	ActiveShowOriginCaptureRequestID.Reset();
	ActiveShowOriginCaptureRevision = 0;
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
		TEXT("候选坐标已手工修改"));
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsSource"),
		TEXT("来源：手动输入"));
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginCaptureTime"),
		TEXT("采集时间：--"));
	UpdateCommandControlState();
}

bool UGCSRouteWorkflowCoordinator::RevalidateShowOriginCandidate(
	FString& OutError) const
{
	OutError.Reset();
	if (!ShowOriginCandidate.bValid || !ShowOriginCandidate.bGpsProvenanceIntact
		|| ShowOriginDraftSource != EGCSShowOriginDraftSource::CapturedCandidate)
	{
		OutError = ShowOriginCandidate.InvalidReason.IsEmpty()
			? TEXT("GPS候选无效，请重新采集或手工修改坐标")
			: ShowOriginCandidate.InvalidReason;
		return false;
	}
	if (!GameInstanceRef.IsValid())
	{
		OutError = TEXT("Route Workflow Coordinator未初始化");
		return false;
	}
	const UGameInstance* GI = GameInstanceRef.Get();
	const UFleetManagerSubsystem* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
	FDroneData Drone;
	if (!Fleet || Fleet->GetActiveFleetSessionID() != ShowOriginCandidate.FleetSessionID
		|| !Fleet->GetDroneByID(ShowOriginCandidate.DroneID, Drone)
		|| !Drone.bOnline || Drone.EndpointIdentity != ShowOriginCandidate.EndpointIdentity)
	{
		OutError = TEXT("GPS候选已失效：Fleet、在线状态或Endpoint已变化");
		return false;
	}
	const URouteGpsCaptureSubsystem* CaptureAuthority =
		GI->GetSubsystem<URouteGpsCaptureSubsystem>();
	FRouteGpsCaptureSnapshot Snapshot;
	if (!CaptureAuthority || !CaptureAuthority->GetCurrentSnapshot(Snapshot)
		|| Snapshot.State != ERouteGpsCaptureState::Ready
		|| Snapshot.CaptureAuthoritySessionID != ShowOriginCandidate.CaptureAuthoritySessionID
		|| Snapshot.CaptureRequestID != ShowOriginCandidate.CaptureRequestID
		|| Snapshot.CaptureRevision != ShowOriginCandidate.CaptureRevision)
	{
		OutError = TEXT("GPS候选已失效：Capture事务已被替换或不再Ready");
		return false;
	}
	return true;
}

bool UGCSRouteWorkflowCoordinator::IsShowOriginMutationBlocked(FString& OutError) const
{
	if (IsConfirmedMappingMutationBlocked(OutError))
	{
		OutError = TEXT("当前航线任务正在执行，不能修改表演原点");
		return true;
	}
	if (GameInstanceRef.IsValid())
	{
		const URouteWaypointGenerationSubsystem* Generator =
			GameInstanceRef->GetSubsystem<URouteWaypointGenerationSubsystem>();
		if (Generator && Generator->IsGenerationActive())
		{
			OutError = TEXT("当前航线任务正在执行，不能修改表演原点");
			return true;
		}
	}
	OutError.Reset();
	return false;
}

bool UGCSRouteWorkflowCoordinator::CommitValidatedShowOrigin(
	const double Latitude,
	const double Longitude,
	const double AltitudeAMSL,
	FString& OutError)
{
	if (!GameInstanceRef.IsValid())
	{
		OutError = TEXT("Route Workflow Coordinator未初始化");
		return false;
	}
	URoutePreviewSubsystem* Preview =
		GameInstanceRef->GetSubsystem<URoutePreviewSubsystem>();
	if (!Preview)
	{
		OutError = TEXT("Show Origin Authority不可用");
		return false;
	}

	FRouteWaypointGeneratorContext NewContext;
	FRouteWaypointGeneratorError NewContextError;
	if (!RouteWaypointGenerator::BuildContext(
		Latitude, Longitude, NewContext, NewContextError))
	{
		OutError = NewContextError.Diagnostic;
		return false;
	}
	const FRoutePreviewData& Current = Preview->GetRouteData();
	const bool bHadCurrentOrigin = Current.bShowOriginSet;
	bool bSameHorizontalOrigin = false;
	if (Current.bShowOriginSet)
	{
		FRouteWaypointGeneratorContext CurrentContext;
		FRouteWaypointGeneratorError CurrentContextError;
		if (!RouteWaypointGenerator::BuildContext(
			Current.ShowOriginLatitude, Current.ShowOriginLongitude,
			CurrentContext, CurrentContextError))
		{
			OutError = TEXT("当前Show Origin Authority包含无效坐标");
			return false;
		}
		bSameHorizontalOrigin =
			CurrentContext.NormalizedShowOriginLatitudeE7 ==
				NewContext.NormalizedShowOriginLatitudeE7
			&& CurrentContext.NormalizedShowOriginLongitudeE7 ==
				NewContext.NormalizedShowOriginLongitudeE7;
		if (bSameHorizontalOrigin && Current.ShowOriginAltitude == AltitudeAMSL)
		{
			OutError.Reset();
			return true;
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	const bool bCommitted = CommandTestHooks.SetOrigin
		? CommandTestHooks.SetOrigin(Latitude, Longitude, AltitudeAMSL, OutError)
		: Preview->SetShowOrigin(Latitude, Longitude, AltitudeAMSL, OutError);
#else
	const bool bCommitted =
		Preview->SetShowOrigin(Latitude, Longitude, AltitudeAMSL, OutError);
#endif
	if (!bCommitted) return false;
	if (!bHadCurrentOrigin || !bSameHorizontalOrigin)
	{
		InvalidateConfirmedNpzMapping(
			TEXT("Confirmed NPZ Mapping was invalidated by a Show Origin change."),
			true);
		ResetAutoStartForNewDataset();
	}
	return true;
}

void UGCSRouteWorkflowCoordinator::HandleApplyShowOriginGpsClicked()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !GameInstanceRef.IsValid()) return;
	FString Error;
	if (IsShowOriginMutationBlocked(Error))
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), Error);
		SetCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview, Error);
		return;
	}
	if (ShowOriginDraftSource == EGCSShowOriginDraftSource::InvalidatedCandidate)
	{
		Error = ShowOriginCandidate.InvalidReason.IsEmpty()
			? TEXT("GPS候选已失效，请重新采集或手工修改坐标")
			: ShowOriginCandidate.InvalidReason;
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), Error);
		return;
	}
	if (ShowOriginDraftSource == EGCSShowOriginDraftSource::CapturedCandidate
		&& !RevalidateShowOriginCandidate(Error))
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), Error);
		return;
	}

	const UEditableTextBox* LatitudeInput = Get<UEditableTextBox>(
		ConvertWidgets, TEXT("EDT_OriginLatitude"));
	const UEditableTextBox* LongitudeInput = Get<UEditableTextBox>(
		ConvertWidgets, TEXT("EDT_OriginLongitude"));
	const UEditableTextBox* AltitudeInput = Get<UEditableTextBox>(
		ConvertWidgets, TEXT("EDT_OriginAltitude"));
	double Latitude = 0.0;
	double Longitude = 0.0;
	double Altitude = 0.0;
	if (!ParseOriginInputs(
		LatitudeInput ? LatitudeInput->GetText().ToString() : FString(),
		LongitudeInput ? LongitudeInput->GetText().ToString() : FString(),
		AltitudeInput ? AltitudeInput->GetText().ToString() : FString(),
		Latitude, Longitude, Altitude, Error))
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"), Error);
		return;
	}

	const bool bFromGps =
		ShowOriginDraftSource == EGCSShowOriginDraftSource::CapturedCandidate;
	const FString AppliedDroneID = ShowOriginCandidate.DroneID;
	{
		TGuardValue<bool> Guard(bCommandInFlight, true);
		if (!CommitValidatedShowOrigin(Latitude, Longitude, Altitude, Error))
		{
			SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
				Error.IsEmpty() ? TEXT("Show Origin Authority拒绝应用") : Error);
			return;
		}
	}
	bShowOriginDraftActive = false;
	ShowOriginDraftSource = EGCSShowOriginDraftSource::Official;
	if (bFromGps)
	{
		ShowOriginCandidate.bApplied = true;
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsSource"), FString::Printf(
			TEXT("来源：%s（已应用）"), *AppliedDroneID));
	}
	else
	{
		SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsSource"),
			TEXT("来源：手动输入（已应用）"));
	}
	SetText(ConvertWidgets, TEXT("TXT_ShowOriginGpsStatus"),
		TEXT("表演原点已应用"));
	ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Preview);
	UE_LOG(LogGCSRouteWorkflow, Display,
		TEXT("[SHOW_ORIGIN_APPLY] Committed Source=%s Lat=%.9f Lon=%.9f AMSL=%.3f"),
		bFromGps ? *AppliedDroneID : TEXT("Manual"), Latitude, Longitude, Altitude);
	RefreshReadOnlyState();
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
		InvalidateConfirmedNpzMapping(
			TEXT("Confirmed NPZ Mapping was invalidated by a successful Route dataset/Origin commit."),
			true);
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
	const FGCSRouteWorkflowCommandGateInputs Inputs = GatherCommandGateInputs();
	const FGCSRouteWorkflowCommandGate Gate = ComputeCommandGate(Inputs);
	if (!Gate.bGenerateWaypointsEnabled)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[RouteWorkflow] Generate click rejected. Prepared=%d Loaded=%d Origin=%d Workspace=%d Generation=%d Command=%d"),
			Inputs.bHasPreparedRoute, Inputs.bRouteLoaded, Inputs.bShowOriginSet,
			Inputs.bWorkspaceReady, Inputs.bGenerationActive, Inputs.bCommandInFlight);
		return;
	}

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
	TMap<FString, const FGCSConfirmedNpzMappingEntry*> ConfirmedEntries;
	TArray<FString> TargetDroneIDs = OutSnapshot.SelectedDroneIDs;
	if (bHasConfirmedNpzMapping)
	{
		if (!ValidateConfirmedNpzMappingCurrent(OutError))
		{
			if (OutError.IsEmpty())
				OutError = TEXT("Confirmed NPZ Mapping is stale; Clear or confirm again.");
			return false;
		}
		if (ConfirmedNpzMapping.SourceDatasetDigest != OutSnapshot.SourceDatasetDigest ||
			ConfirmedNpzMapping.RouteCount != OutSnapshot.RouteSlotCount)
		{
			OutError = TEXT("Current Artifact does not match the Confirmed NPZ Mapping dataset/domain.");
			return false;
		}
		TargetDroneIDs.Reset(ConfirmedNpzMapping.Entries.Num());
		for (const FGCSConfirmedNpzMappingEntry& Entry : ConfirmedNpzMapping.Entries)
		{
			TargetDroneIDs.Add(Entry.DroneID);
			ConfirmedEntries.Add(Entry.DroneID, &Entry);
		}
		TargetDroneIDs.Sort();
		OutSnapshot.bUsesConfirmedNpzMapping = true;
		OutSnapshot.ConfirmedMappingRevision = ConfirmedNpzMapping.ConfirmationRevision;
	}
	if (TargetDroneIDs.IsEmpty())
	{
		OutError = bHasConfirmedNpzMapping
			? TEXT("Confirmed NPZ Mapping contains no executable matched drones; abnormal drones are excluded from upload.")
			: TEXT("No drones selected.");
		return false;
	}
	// A confirmed mapping owns the upload target set. Keep the captured snapshot
	// coherent even when the operator has since cleared or changed UI selection.
	OutSnapshot.SelectedDroneIDs = TargetDroneIDs;

	TSet<int32> NumericIDs;
	TSet<int32> RouteSlots;
	OutSnapshot.Targets.Reserve(TargetDroneIDs.Num());
	for (const FString& DroneID : TargetDroneIDs)
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
		const FGCSConfirmedNpzMappingEntry* const* ConfirmedEntry =
			ConfirmedEntries.Find(DroneID);
		const int32 RouteSlot = ConfirmedEntry ? (*ConfirmedEntry)->RouteSlotIndex : NumericID;
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
		if (ConfirmedEntry && Drone.EndpointIdentity != (*ConfirmedEntry)->EndpointIdentity)
		{
			OutError = FString::Printf(
				TEXT("Confirmed NPZ Mapping endpoint changed for %s."), *DroneID);
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
	Candidate.MaxConcurrentTargets = AppliedMaxConcurrentTargets;
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
		dspgcs::UploadRuntimeConfigBindingDTO ConfigBinding;
		if (!Upload->CaptureMavFtpRuntimeConfigBinding(ConfigBinding, OutError))
		{
			return false;
		}
		Candidate.ConfigAuthoritySessionID =
			GCSRouteWorkflowPrivate::WorkflowFromUtf8(
				ConfigBinding.authoritySessionId);
		Candidate.ConfigRevision = static_cast<int64>(
			ConfigBinding.configRevision);
		Candidate.ConfigDigest = GCSRouteWorkflowPrivate::WorkflowDigestHex(
			ConfigBinding.configDigest);
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
		TEXT("[ROUTE_AUTO_START] Transaction=%s State=SyncingConsole Targets=%d MaxConcurrentTargets=%d"),
		*AutoStartSnapshot.TransactionID, AutoStartSnapshot.Targets.Num(),
		AutoStartSnapshot.MaxConcurrentTargets);
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
		AppliedMaxConcurrentTargets != AutoStartSnapshot.MaxConcurrentTargets ||
		Current.FleetSessionID != AutoStartSnapshot.FleetSessionID ||
		(!AutoStartSnapshot.bUsesConfirmedNpzMapping &&
			Current.FleetRevision != AutoStartSnapshot.FleetRevision) ||
		Current.ArtifactSetID != AutoStartSnapshot.ArtifactSetID ||
		Current.ArtifactSetDigest != AutoStartSnapshot.ArtifactSetDigest ||
		Current.SourceDatasetDigest != AutoStartSnapshot.SourceDatasetDigest ||
		Current.RouteSlotCount != AutoStartSnapshot.RouteSlotCount ||
		Current.Targets.Num() != AutoStartSnapshot.Targets.Num() ||
		Current.bUsesConfirmedNpzMapping != AutoStartSnapshot.bUsesConfirmedNpzMapping ||
		Current.ConfirmedMappingRevision != AutoStartSnapshot.ConfirmedMappingRevision)
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

	const UDSPGCSUploadJobSubsystem* UploadConfig =
		GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	dspgcs::UploadRuntimeConfigBindingDTO CurrentConfigBinding;
	if (!UploadConfig || !UploadConfig->CaptureMavFtpRuntimeConfigBinding(
			CurrentConfigBinding, OutError) ||
		GCSRouteWorkflowPrivate::WorkflowFromUtf8(
			CurrentConfigBinding.authoritySessionId) !=
			AutoStartSnapshot.ConfigAuthoritySessionID ||
		static_cast<int64>(CurrentConfigBinding.configRevision) !=
			AutoStartSnapshot.ConfigRevision ||
		GCSRouteWorkflowPrivate::WorkflowDigestHex(
			CurrentConfigBinding.configDigest) != AutoStartSnapshot.ConfigDigest)
	{
		if (OutError.IsEmpty())
			OutError = TEXT("MAVFTP Runtime Config identity changed during AutoStart.");
		return false;
	}

	const URouteAssignmentSubsystem* Assignment =
		GameInstanceRef->GetSubsystem<URouteAssignmentSubsystem>();
	FRouteAssignmentSourceSnapshot Source;
	FRouteAssignmentSnapshot Frozen;
	if (!Assignment || !Assignment->CaptureSourceSnapshot(Source, OutError) ||
		!(AutoStartSnapshot.bUsesConfirmedNpzMapping
			? GCSRouteWorkflowPrivate::SameStableAssignmentSource(
				Source.SourceIdentity, AutoStartSnapshot.AssignmentSourceIdentity)
			: GCSRouteWorkflowPrivate::SameAssignmentSource(
				Source.SourceIdentity, AutoStartSnapshot.AssignmentSourceIdentity)) ||
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
		bStarted = AutoStartTestHooks.StartJob(
			AutoStartSnapshot.MaxConcurrentTargets, UploadJobID, Error);
	}
	else
#endif
	{
		UDSPGCSUploadJobSubsystem* Upload =
			GameInstanceRef->GetSubsystem<UDSPGCSUploadJobSubsystem>();
		dspgcs::UploadRuntimeConfigBindingDTO ExpectedConfig;
		ExpectedConfig.authoritySessionId =
			GCSRouteWorkflowPrivate::WorkflowToUtf8(
				AutoStartSnapshot.ConfigAuthoritySessionID);
		ExpectedConfig.configRevision = AutoStartSnapshot.ConfigRevision > 0
			? static_cast<std::uint64_t>(AutoStartSnapshot.ConfigRevision) : 0;
		if (!GCSRouteWorkflowPrivate::WorkflowParseDigestHex(
				AutoStartSnapshot.ConfigDigest, ExpectedConfig.configDigest))
		{
			Error = TEXT("AutoStart Runtime Config digest is invalid.");
		}
		else
		{
			bStarted = Upload && Upload->StartUploadJobFromCurrentAuthorities(
				AutoStartSnapshot.MaxConcurrentTargets, ExpectedConfig,
				UploadJobID, Error);
		}
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
		TEXT("[ROUTE_AUTO_START] Transaction=%s State=Completed UploadJobID=%s Targets=%d MaxConcurrentTargets=%d"),
		*AutoStartSnapshot.TransactionID, *UploadJobID,
		AutoStartSnapshot.Targets.Num(), AutoStartSnapshot.MaxConcurrentTargets);
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
	AppliedMaxConcurrentTargets = Maximum;
	ClearCommandError(EGCSRouteWorkflowCommandErrorCategory::Upload);
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
	{
		Button->SetIsEnabled(Gate.bGenerateWaypointsEnabled);
		FString Hint = TEXT("Generate and validate a new Waypoints package for the loaded NPZ.");
		if (!Gate.bGenerateWaypointsEnabled)
		{
			if (Inputs.bGenerationActive) Hint = TEXT("Waypoints generation is already running.");
			else if (Inputs.bCommandInFlight) Hint = TEXT("Another route command is still running.");
			else if (Inputs.bHasPreparedRoute) Hint = TEXT("Load/commit the selected NPZ preview before generation.");
			else if (!Inputs.bRouteLoaded) Hint = TEXT("Import an NPZ route before generation.");
			else if (!Inputs.bShowOriginSet) Hint = TEXT("Set the Show Origin before generation.");
			else if (!Inputs.bWorkspaceReady)
			{
				const URouteArtifactWorkspaceSubsystem* Workspace = GameInstanceRef.IsValid()
					? GameInstanceRef->GetSubsystem<URouteArtifactWorkspaceSubsystem>() : nullptr;
				Hint = Workspace
					? TEXT("Artifact Workspace is not ready: ")
						+ Workspace->GetLastRecoveryResult().RecoverySummary
					: TEXT("Artifact Workspace is unavailable.");
			}
		}
		Button->SetToolTipText(FText::FromString(Hint));
	}
	FString NumberingGateError;
	const bool bNumberingMutationAllowed =
		!IsConfirmedMappingMutationBlocked(NumberingGateError);
	FRouteGpsCaptureSnapshot Capture;
	const URouteGpsCaptureSubsystem* CaptureAuthority = GameInstanceRef.IsValid()
		? GameInstanceRef->GetSubsystem<URouteGpsCaptureSubsystem>() : nullptr;
	const bool bHasCapture = CaptureAuthority &&
		CaptureAuthority->GetLatestCommandSelectionSnapshot(Capture);
	TArray<FString> CurrentSelection;
	if (const UGCSUIBridgeSubsystem* Bridge = GameInstanceRef.IsValid()
		? GameInstanceRef->GetSubsystem<UGCSUIBridgeSubsystem>() : nullptr)
	{
		CurrentSelection = Bridge->GetSelectedCommandDroneIDs();
		CurrentSelection.Sort();
	}
	const bool bCaptureReadyForSelection = bHasCapture &&
		Capture.State == ERouteGpsCaptureState::Ready &&
		Capture.SelectedDroneIDs == CurrentSelection;
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_CaptureStableGps")))
	{
		Button->SetIsEnabled(bNumberingMutationAllowed && Inputs.bRouteLoaded &&
			Inputs.bShowOriginSet && !CurrentSelection.IsEmpty() &&
			(!bHasCapture || Capture.State != ERouteGpsCaptureState::Capturing));
		Button->SetToolTipText(FText::FromString(
			bNumberingMutationAllowed
				? TEXT("Capture a selection-bound stable GPS window. This does not assign NPZ RouteSlots.")
				: NumberingGateError));
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ApplyNpzNumbering")))
	{
		Button->SetIsEnabled(bNumberingMutationAllowed && Inputs.bRouteLoaded &&
			Inputs.bShowOriginSet && bCaptureReadyForSelection);
		Button->SetToolTipText(FText::FromString(
			bNumberingMutationAllowed
				? (!bCaptureReadyForSelection
					? TEXT("F.1 Stable GPS Capture must be Ready for the current Selection first.")
					: TEXT("Confirm direct one-to-one NPZ RouteSlot matching in the fixed Show Origin ENU / Frame-0 coordinate system."))
				: NumberingGateError));
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ClearNpzNumbering")))
	{
		Button->SetIsEnabled(bNumberingMutationAllowed && bHasConfirmedNpzMapping);
		Button->SetToolTipText(FText::FromString(
			bNumberingMutationAllowed
				? TEXT("Clear the Confirmed NPZ Mapping and its visualization override.")
				: NumberingGateError));
	}
	for (const FName Name : {FName(TEXT("EDT_OriginLatitude")),
		FName(TEXT("EDT_OriginLongitude")), FName(TEXT("EDT_OriginAltitude"))})
	{
		if (UEditableTextBox* Box = Get<UEditableTextBox>(ConvertWidgets, Name))
		{
			Box->SetIsReadOnly(Inputs.bGenerationActive || Inputs.bCommandInFlight);
			Box->SetIsEnabled(!Inputs.bGenerationActive && !Inputs.bCommandInFlight);
		}
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_RefreshShowOriginDrones")))
	{
		Button->SetIsEnabled(!bCommandInFlight);
	}
	FRouteGpsCaptureSnapshot ActiveCapture;
	const bool bAnyCaptureRunning = CaptureAuthority
		&& CaptureAuthority->GetCurrentSnapshot(ActiveCapture)
		&& ActiveCapture.State == ERouteGpsCaptureState::Capturing;
	bool bSelectedAnchorOnline = false;
	if (!SelectedShowOriginDroneID.IsEmpty())
	{
		FDroneData Anchor;
		const UFleetManagerSubsystem* Fleet = GameInstanceRef.IsValid()
			? GameInstanceRef->GetSubsystem<UFleetManagerSubsystem>() : nullptr;
		bSelectedAnchorOnline = Fleet
			&& Fleet->GetDroneByID(SelectedShowOriginDroneID, Anchor)
			&& Anchor.bOnline;
	}
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_CaptureShowOriginGps")))
	{
		Button->SetIsEnabled(!bCommandInFlight && bSelectedAnchorOnline
			&& !bAnyCaptureRunning);
	}
	FString ShowOriginBlockReason;
	const bool bShowOriginBlocked = IsShowOriginMutationBlocked(ShowOriginBlockReason);
	const bool bDraftUsable = ShowOriginDraftSource !=
		EGCSShowOriginDraftSource::InvalidatedCandidate;
	if (UButton* Button = Get<UButton>(ConvertWidgets, TEXT("BTN_ApplyShowOriginGps")))
	{
		Button->SetIsEnabled(!bShowOriginBlocked && Inputs.bOriginValid && bDraftUsable);
		Button->SetToolTipText(FText::FromString(bShowOriginBlocked
			? ShowOriginBlockReason
			: (bDraftUsable ? TEXT("验证并原子应用当前经纬度和AMSL为正式表演原点")
				: TEXT("GPS候选已失效，请重新采集或手工修改坐标"))));
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
			Gate.bStartEnabled ? FString::Printf(
				TEXT("Automatic selected-drone assignment; applied concurrency = %d."),
				AppliedMaxConcurrentTargets)
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
	RefreshShowOriginCaptureState();
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
		if (!Prepared && Origin && !bShowOriginDraftActive)
		{
			SetShowOriginDraftFields(Data.ShowOriginLatitude,
				Data.ShowOriginLongitude, Data.ShowOriginAltitude);
		}
		FString PreviewText = Preview->IsPreviewModeActive() ? TEXT("Preview: ")+Playback(Preview->GetPlaybackState()) : (Preview->IsRouteOverlayVisible() ? TEXT("Live + Frame-0 Overlay") : (Preview->IsRouteOverlayEnabled() ? TEXT("Overlay Enabled") : TEXT("--")));
		SetText(ConvertWidgets,TEXT("TXT_StatePreviewValue"),PreviewText);
	}
	FRouteGpsCaptureSnapshot GpsCapture;
	const URouteGpsCaptureSubsystem* GpsCaptureAuthority =
		GI->GetSubsystem<URouteGpsCaptureSubsystem>();
	const bool bHasGpsCapture = GpsCaptureAuthority &&
		GpsCaptureAuthority->GetLatestCommandSelectionSnapshot(GpsCapture);
	int32 GpsReady = 0;
	int32 GpsPending = 0;
	int32 GpsFailed = 0;
	TArray<FString> GpsExceptions;
	if (bHasGpsCapture)
	{
		for (const FRouteGpsCaptureParticipantResult& Participant : GpsCapture.Participants)
		{
			switch (Participant.Status)
			{
			case ERouteGpsCaptureParticipantStatus::Ready: ++GpsReady; break;
			case ERouteGpsCaptureParticipantStatus::Pending: ++GpsPending; break;
			case ERouteGpsCaptureParticipantStatus::Failed: ++GpsFailed; break;
			default: break;
			}
			if (Participant.FailureReason != ERouteGpsCaptureFailureReason::None)
			{
				GpsExceptions.Add(FString::Printf(TEXT("%s: %s"),
					*Participant.DroneID,
					*GpsCaptureFailure(Participant.FailureReason)));
			}
		}
	}
	const int32 MatchedCount = bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid
		? ConfirmedNpzMapping.Entries.Num() : 0;
	const int32 AbnormalCount = bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid
		? ConfirmedNpzMapping.UnassignedDroneIDs.Num() : 0;
	int32 RouteSlotCount = 0;
	if (const URoutePreviewSubsystem* Preview = GI->GetSubsystem<URoutePreviewSubsystem>())
	{
		RouteSlotCount = Preview->HasPreparedRoute()
			? Preview->GetPreparedDroneCount() : Preview->GetDroneCount();
	}
	const int32 VacantCount = bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid
		? ConfirmedNpzMapping.VacantRouteSlots.Num()
		: FMath::Max(0, RouteSlotCount - MatchedCount);
	const int32 SpareCount = bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid
		? ConfirmedNpzMapping.SpareDroneIDs.Num() : 0;
	const FString MappingState = bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid
		? (AbnormalCount > 0 ? TEXT("Confirmed / Degraded") : TEXT("Confirmed"))
		: (bHasGpsCapture ? GpsCaptureState(GpsCapture.State) : TEXT("Not Captured"));
	SetText(ConvertWidgets, TEXT("TXT_NpzGpsMappingStatusValue"), MappingState);
	SetText(ConvertWidgets, TEXT("TXT_NpzGpsMappingCounts"), FString::Printf(
		TEXT("GPS Ready %d/%d | Matched %d | Vacant %d | Spare %d | Abnormal %d"),
		GpsReady, bHasGpsCapture ? GpsCapture.SelectedDroneIDs.Num() : 0,
		MatchedCount, VacantCount, SpareCount, AbnormalCount));
	TArray<FString> MappingDetails = GpsExceptions;
	if (bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid)
	{
		for (const FGCSConfirmedNpzMappingEntry& Match : ConfirmedNpzMapping.Entries)
		{
			MappingDetails.Add(FString::Printf(TEXT("%s -> Slot %d (%.3f m)"),
				*Match.DroneID, Match.RouteSlotIndex, Match.MatchDistanceMeters));
		}
		for (const FString& DroneID : ConfirmedNpzMapping.UnassignedDroneIDs)
		{
			MappingDetails.Add(FString::Printf(
				TEXT("%s -> ABNORMAL (no RouteSlot; upload skipped)"), *DroneID));
		}
	}
	const FString MappingDetailsText = MappingDetails.IsEmpty()
		? TEXT("Exceptions/Matches: none")
		: FString::Join(MappingDetails, TEXT("; "));
	SetText(ConvertWidgets, TEXT("TXT_NpzMappingExceptions"), MappingDetailsText);
	SetTooltip(ConvertWidgets, TEXT("TXT_NpzMappingExceptions"), MappingDetailsText);
	FRouteGpsCapturePolicy ConfiguredGpsPolicy;
	FString GpsPolicyError;
	const bool bGpsPolicyValid = URouteGpsCaptureSubsystem::LoadConfiguredPolicy(
		ConfiguredGpsPolicy, GpsPolicyError);
	FString MappingDescription = TEXT("Capture stable GPS, then directly match ENU positions to the externally generated NPZ Frame-0 RouteSlots.");
	if (bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid)
	{
		MappingDescription = FString::Printf(
			TEXT("Confirmed fixed-frame NPZ Mapping: %d executable | %d abnormal/skipped | Revision %lld"),
			ConfirmedNpzMapping.Entries.Num(), ConfirmedNpzMapping.UnassignedDroneIDs.Num(),
			ConfirmedNpzMapping.ConfirmationRevision);
	}
	SetText(ConvertWidgets, TEXT("TXT_NpzGpsMappingDescription"), MappingDescription);
	SetTooltip(ConvertWidgets, TEXT("TXT_NpzGpsMappingDescription"), MappingDescription);
	FString MappingWarning;
	if (!bGpsPolicyValid) MappingWarning = TEXT("GPS policy invalid: ") + GpsPolicyError;
	else if (!ConfiguredGpsPolicy.bHardwareValidated)
	{
		MappingWarning = FString::Printf(
			TEXT("SIMULATION/COMMISSIONING - NOT HARDWARE VALIDATED: GPS=%s"),
			*ConfiguredGpsPolicy.ProfileName);
	}
	else MappingWarning = FString::Printf(TEXT("Hardware-validated GPS profile: %s"),
		*ConfiguredGpsPolicy.ProfileName);
	if (AbnormalCount > 0)
	{
		MappingWarning += FString::Printf(
			TEXT(" | DEGRADED: %d abnormal drone(s) are red, unnumbered, and excluded from upload."),
			AbnormalCount);
	}
	SetText(ConvertWidgets, TEXT("TXT_NpzMappingWarning"), MappingWarning);
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
	const uint64 Total=Job?Job->totalBytes:0,Done=Job?Job->bytesCompleted:0; const double UR=ComputeProgressRatio(Done,Total); SetText(UploadWidgets,TEXT("TXT_UploadThroughputValue"),FormatThroughput(Job?Job->throughputBytesPerSecond:0)); SetText(UploadWidgets,TEXT("TXT_UploadBytesValue"),FormatLogicalByteProgress(Done,Total)); SetText(UploadWidgets,TEXT("TXT_UploadElapsedValue"),FormatElapsedMilliseconds(Job?Job->elapsedMs:0)); if(UProgressBar* Bar=Get<UProgressBar>(UploadWidgets,TEXT("PROGRESS_Upload"))) Bar->SetPercent(UR); SetText(UploadWidgets,TEXT("TXT_UploadPercent"),FormatPercent(UR)); SetText(UploadWidgets,TEXT("TXT_CurrentMaxConcurrencyValue"),FString::Printf(TEXT("Current: %d"),AppliedMaxConcurrentTargets));
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

bool UGCSRouteWorkflowCoordinator::TickRefresh(float DeltaSeconds)
{
	(void)DeltaSeconds;
	AdvanceAutoStartTransaction();
	if (bHasConfirmedNpzMapping && ConfirmedNpzMapping.bValid &&
		!IsCommandSelectionMutationBlocked())
	{
		FString InvalidReason;
		if (!ValidateConfirmedNpzMappingCurrent(InvalidReason))
		{
			InvalidateConfirmedNpzMapping(InvalidReason, true);
		}
	}
	if (RouteWorkflowWidget && RouteWorkflowWidget->IsVisible())
	{
		RefreshReadOnlyState();
	}
	return true;
}
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
	if (Value > 50)
	{
		OutError = TEXT("Maximum concurrency must be in range 1..50.");
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
		Gate.bApplyMaxEnabled = !Inputs.bAutoStartInFlight;
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
		break;
	case dspgcs::UploadJobState::Paused:
		Gate.bResumeEnabled = true;
		Gate.bCancelEnabled = true;
		break;
	case dspgcs::UploadJobState::Failed:
		Gate.bCancelEnabled = true;
		Gate.bRetryFailedEnabled = Inputs.FailedCount > 0;
		Gate.bApplyMaxEnabled = true;
		break;
	case dspgcs::UploadJobState::Completed:
	case dspgcs::UploadJobState::Cancelled:
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
