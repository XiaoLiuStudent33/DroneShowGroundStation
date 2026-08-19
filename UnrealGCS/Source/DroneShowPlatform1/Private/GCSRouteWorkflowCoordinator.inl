#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "DSPGCSUploadJobSubsystem.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
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
			Result.Emplace(FName(Name), Type);
		}
		return Result;
	}

	FString AssignmentState(const ERouteAssignmentState V)
	{
		switch (V) { case ERouteAssignmentState::Empty:return TEXT("Empty"); case ERouteAssignmentState::Draft:return TEXT("Draft"); case ERouteAssignmentState::Ready:return TEXT("Ready"); case ERouteAssignmentState::Frozen:return TEXT("Frozen"); default:return TEXT("--"); }
	}
	FString Readiness(const ERouteAssignmentReadinessKind V)
	{
		switch (V) { case ERouteAssignmentReadinessKind::FullReady:return TEXT("Full Ready"); case ERouteAssignmentReadinessKind::DegradedReady:return TEXT("Degraded Ready"); default:return TEXT("Not Ready"); }
	}
	FString Completion(const ERouteAssignmentCompletionMode V)
	{
		return V == ERouteAssignmentCompletionMode::Full ? TEXT("Full") : (V == ERouteAssignmentCompletionMode::Degraded ? TEXT("Degraded") : TEXT("Unset"));
	}
	FString Sync(const EDSPGCSFrozenAssignmentSyncState V)
	{
		switch (V) { case EDSPGCSFrozenAssignmentSyncState::FrozenLocal:return TEXT("Frozen Local"); case EDSPGCSFrozenAssignmentSyncState::SyncingToConsole:return TEXT("Syncing"); case EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted:return TEXT("Accepted"); case EDSPGCSFrozenAssignmentSyncState::SyncFailed:return TEXT("Sync Failed"); default:return TEXT("Not Frozen"); }
	}
	FString Current(const ERouteArtifactCurrentStatus V)
	{
		return V == ERouteArtifactCurrentStatus::Ready ? TEXT("Ready") : (V == ERouteArtifactCurrentStatus::Invalid ? TEXT("Invalid") : TEXT("None"));
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
		return C == ERouteArtifactCurrentStatus::Ready ? TEXT("READY") : (C == ERouteArtifactCurrentStatus::Invalid ? TEXT("INVALID") : TEXT("NOT READY"));
	}
	FString JobState(const dspgcs::UploadJobState V)
	{
		switch (V) { case dspgcs::UploadJobState::Created:return TEXT("Created"); case dspgcs::UploadJobState::Validating:return TEXT("Validating"); case dspgcs::UploadJobState::Ready:return TEXT("Ready"); case dspgcs::UploadJobState::Running:return TEXT("Running"); case dspgcs::UploadJobState::Paused:return TEXT("Paused"); case dspgcs::UploadJobState::Cancelling:return TEXT("Cancelling"); case dspgcs::UploadJobState::Completed:return TEXT("Completed"); case dspgcs::UploadJobState::Failed:return TEXT("Failed"); case dspgcs::UploadJobState::Cancelled:return TEXT("Cancelled"); default:return TEXT("--"); }
	}
	FString Gate(const dspgcs::UploadArtifactGateState V)
	{
		switch (V) { case dspgcs::UploadArtifactGateState::NotChecked:return TEXT("Not Checked"); case dspgcs::UploadArtifactGateState::Validating:return TEXT("Validating"); case dspgcs::UploadArtifactGateState::Validated:return TEXT("Validated"); case dspgcs::UploadArtifactGateState::Rejected:return TEXT("Rejected"); default:return TEXT("--"); }
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
	if (UButton* Button = OpenWorkflowButtonRef.Get()) Button->OnClicked.RemoveDynamic(this, &UGCSRouteWorkflowCoordinator::HandleRouteWorkflowClicked);
	if (RouteWorkflowWidget) RouteWorkflowWidget->RemoveFromParent();
	RouteWorkflowWidget = nullptr;
	RouteConvertWidgetRef.Reset(); RouteUploadWidgetRef.Reset();
	ConvertWidgets.Reset(); UploadWidgets.Reset();
	OpenWorkflowButtonRef.Reset(); RightPanelWidgetRef.Reset(); GameInstanceRef.Reset();
	bBindingValid = false;
#if WITH_DEV_AUTOMATION_TESTS
	WorkflowFactoryForTest = {};
	bSkipRequiredWidgetBindingForTest = false;
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
		TEXT("EDT_OriginLatitude"),TEXT("EDT_OriginLongitude"),TEXT("EDT_OriginAltitude"),
		TEXT("TXT_AssignmentState"),TEXT("TXT_AssignmentMode"),TEXT("TXT_ConsoleAccepted"),
		TEXT("TXT_MatchedCount"),TEXT("TXT_VacantCount"),TEXT("TXT_SpareCount"),TEXT("TXT_OutputPath"),
		TEXT("TXT_StateDatasetValue"),TEXT("TXT_StatePreviewValue"),TEXT("TXT_StateAssignmentValue"),TEXT("TXT_StateArtifactValue"),
		TEXT("PROGRESS_Convert"),TEXT("TXT_GeneratedCount"),TEXT("TXT_ValidatedCount"),TEXT("TXT_ConvertPercent"),
		TEXT("TXT_ArtifactReadyValue"),TEXT("TXT_ArtifactIdValue"),TEXT("TXT_ArtifactElapsedValue")}), ConvertWidgets, OutError)) return false;
	if (!ResolveRequiredWidgets(Upload, Required({
		TEXT("TXT_ArtifactReadyValue"),TEXT("TXT_ConsoleValidatedValue"),TEXT("TXT_UploadMatched"),TEXT("TXT_UploadVacant"),TEXT("TXT_UploadSpare"),
		TEXT("TXT_UploadWaiting"),TEXT("TXT_UploadPreparing"),TEXT("TXT_UploadUploading"),TEXT("TXT_UploadRetryWaiting"),
		TEXT("TXT_UploadSucceeded"),TEXT("TXT_UploadFailed"),TEXT("TXT_UploadCancelled"),TEXT("TXT_UploadJobStateValue"),
		TEXT("TXT_UploadThroughputValue"),TEXT("TXT_UploadBytesValue"),TEXT("TXT_UploadElapsedValue"),TEXT("PROGRESS_Upload"),
		TEXT("TXT_UploadPercent"),TEXT("EDT_MaxConcurrency"),TEXT("BTN_StartUpload"),TEXT("BTN_PauseUpload"),
		TEXT("BTN_ResumeUpload"),TEXT("BTN_CancelUpload"),TEXT("BTN_RetryFailed")}), UploadWidgets, OutError)) return false;
	RouteConvertWidgetRef = Convert; RouteUploadWidgetRef = Upload;
	ApplyReadOnlyControlState(); bBindingValid = true; return true;
}

void UGCSRouteWorkflowCoordinator::ApplyReadOnlyControlState()
{
	using namespace GCSRouteWorkflowPrivate;
	for (const FName Name : {FName(TEXT("BTN_StartUpload")),FName(TEXT("BTN_PauseUpload")),FName(TEXT("BTN_ResumeUpload")),FName(TEXT("BTN_CancelUpload")),FName(TEXT("BTN_RetryFailed"))})
		if (UButton* Button = Get<UButton>(UploadWidgets, Name)) Button->SetIsEnabled(false);
	if (UEditableTextBox* Box = Get<UEditableTextBox>(UploadWidgets, TEXT("EDT_MaxConcurrency"))) { Box->SetIsReadOnly(true); Box->SetIsEnabled(false); }
	for (const FName Name : {FName(TEXT("EDT_OriginLatitude")),FName(TEXT("EDT_OriginLongitude")),FName(TEXT("EDT_OriginAltitude"))})
		if (UEditableTextBox* Box = Get<UEditableTextBox>(ConvertWidgets, Name)) Box->SetIsReadOnly(true);
	if (UUserWidget* Convert = RouteConvertWidgetRef.Get())
		for (const FName Name : {FName(TEXT("BTN_RouteBrowse")),FName(TEXT("BTN_OutputBrowse")),FName(TEXT("BTN_LoadPreview")),FName(TEXT("BTN_GenerateWaypoints"))})
			if (UButton* Button = Convert->WidgetTree ? Cast<UButton>(Convert->WidgetTree->FindWidget(Name)) : nullptr) Button->SetIsEnabled(false);
}

void UGCSRouteWorkflowCoordinator::RefreshReadOnlyState()
{
	using namespace GCSRouteWorkflowPrivate;
	if (!IsInGameThread() || !bBindingValid || !GameInstanceRef.IsValid()) return;
	UGameInstance* GI = GameInstanceRef.Get();
	if (URoutePreviewSubsystem* Preview = GI->GetSubsystem<URoutePreviewSubsystem>())
	{
		const bool Loaded = Preview->IsRouteLoaded(), Prepared = Preview->HasPreparedRoute();
		const FString Dataset = Loaded ? TEXT("Loaded") : (Prepared ? TEXT("Prepared") : TEXT("--"));
		SetText(ConvertWidgets,TEXT("TXT_DatasetState"),Dataset); SetText(ConvertWidgets,TEXT("TXT_StateDatasetValue"),Dataset);
		SetText(ConvertWidgets,TEXT("TXT_RouteCount"),FString::FromInt(Preview->GetDroneCount())); SetText(ConvertWidgets,TEXT("TXT_FrameCount"),FString::FromInt(Preview->GetFrameCount()));
		const FRouteArtifactSourceIdentity* ID = Loaded ? &Preview->GetActiveSourceIdentity() : (Prepared ? &Preview->GetPreparedSourceIdentity() : nullptr);
		SetText(ConvertWidgets,TEXT("TXT_RouteFilePath"),ID && !ID->SourceDisplayFileName.IsEmpty() ? ID->SourceDisplayFileName : TEXT("--"));
		const FRoutePreviewData& Data = Preview->GetRouteData(); const bool Origin = Loaded && Data.bShowOriginSet;
		SetEdit(ConvertWidgets,TEXT("EDT_OriginLatitude"),Origin ? FString::Printf(TEXT("%.8f"),Data.ShowOriginLatitude):TEXT("--"));
		SetEdit(ConvertWidgets,TEXT("EDT_OriginLongitude"),Origin ? FString::Printf(TEXT("%.8f"),Data.ShowOriginLongitude):TEXT("--"));
		SetEdit(ConvertWidgets,TEXT("EDT_OriginAltitude"),Origin ? FString::Printf(TEXT("%.3f"),Data.ShowOriginAltitude):TEXT("--"));
		FString PreviewText = Preview->IsPreviewModeActive() ? TEXT("Preview: ")+Playback(Preview->GetPlaybackState()) : (Preview->IsRouteOverlayVisible() ? TEXT("Live + Frame-0 Overlay") : (Preview->IsRouteOverlayEnabled() ? TEXT("Overlay Enabled") : TEXT("--")));
		SetText(ConvertWidgets,TEXT("TXT_StatePreviewValue"),PreviewText);
	}
	URouteAssignmentSubsystem* Assignment = GI->GetSubsystem<URouteAssignmentSubsystem>(); FRouteAssignmentSnapshot AS;
	const ERouteAssignmentState AState = Assignment ? Assignment->GetState() : ERouteAssignmentState::Empty;
	const ERouteAssignmentReadinessKind AReady = Assignment ? Assignment->GetReadinessKind() : ERouteAssignmentReadinessKind::NotReady;
	const bool HasAS = Assignment && (Assignment->GetFrozenSnapshot(AS) || Assignment->GetWorkingSnapshot(AS));
	const FString StateText = AssignmentState(AState), ModeText = HasAS && AS.CompletionMode != ERouteAssignmentCompletionMode::Unset ? Completion(AS.CompletionMode) : Readiness(AReady);
	const UDSPGCSFrozenAssignmentSyncSubsystem* Frozen = GI->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>(); const FString SyncText = Sync(Frozen ? Frozen->GetSyncState() : EDSPGCSFrozenAssignmentSyncState::NotFrozen);
	SetText(ConvertWidgets,TEXT("TXT_AssignmentState"),StateText); SetText(ConvertWidgets,TEXT("TXT_AssignmentMode"),ModeText); SetText(ConvertWidgets,TEXT("TXT_ConsoleAccepted"),SyncText);
	SetText(ConvertWidgets,TEXT("TXT_MatchedCount"),FString::FromInt(HasAS ? AS.Summary.MatchedCount:0)); SetText(ConvertWidgets,TEXT("TXT_VacantCount"),FString::FromInt(HasAS ? AS.Summary.VacantRouteSlotCount:0)); SetText(ConvertWidgets,TEXT("TXT_SpareCount"),FString::FromInt(HasAS ? AS.Summary.SpareCount:0));
	SetText(ConvertWidgets,TEXT("TXT_StateAssignmentValue"),FString::Printf(TEXT("%s / %s / %s"),*StateText,*Readiness(AReady),*SyncText));
	URouteArtifactWorkspaceSubsystem* Workspace = GI->GetSubsystem<URouteArtifactWorkspaceSubsystem>(); ERouteArtifactCurrentStatus C=ERouteArtifactCurrentStatus::None; ERouteArtifactGenerationStatus G=ERouteArtifactGenerationStatus::None; ERouteArtifactRecoveryStatus R=ERouteArtifactRecoveryStatus::Stable; FRouteArtifactCurrentInfo Info; FString Root;
	if (Workspace) { C=Workspace->GetCurrentStatus(); G=Workspace->GetGenerationStatus(); R=Workspace->GetLastRecoveryResult().Status; Info=Workspace->GetCurrentInfo(); FString Error; if(!Workspace->ResolveWorkspaceRoot(Root,Error)) Root.Reset(); }
	SetText(ConvertWidgets,TEXT("TXT_OutputPath"),Root.IsEmpty()?TEXT("--"):Root); const FString ReadyText=Ready(C,R); SetText(ConvertWidgets,TEXT("TXT_ArtifactReadyValue"),ReadyText); SetText(UploadWidgets,TEXT("TXT_ArtifactReadyValue"),ReadyText);
	SetText(ConvertWidgets,TEXT("TXT_ArtifactIdValue"),C==ERouteArtifactCurrentStatus::Ready&&!Info.ArtifactSetID.IsEmpty()?Info.ArtifactSetID:TEXT("--")); SetText(ConvertWidgets,TEXT("TXT_StateArtifactValue"),FString::Printf(TEXT("Current: %s / Generation: %s / Recovery: %s"),*Current(C),*Generation(G),*Recovery(R)));
	FRouteWaypointBatchProgress GP; if(const URouteWaypointGenerationSubsystem* Generator=GI->GetSubsystem<URouteWaypointGenerationSubsystem>()) GP=Generator->GetProgress();
	const double GR=ComputeProgressRatio(FMath::Max(0,GP.CompletedRoutes),FMath::Max(0,GP.TotalRoutes)); if(UProgressBar* Bar=Get<UProgressBar>(ConvertWidgets,TEXT("PROGRESS_Convert"))) Bar->SetPercent(GR);
	SetText(ConvertWidgets,TEXT("TXT_GeneratedCount"),FString::Printf(TEXT("%d / %d"),GP.CompletedRoutes,GP.TotalRoutes)); SetText(ConvertWidgets,TEXT("TXT_ValidatedCount"),C==ERouteArtifactCurrentStatus::Ready?FString::Printf(TEXT("%d / %d"),Info.RouteSlotCount,Info.RouteSlotCount):TEXT("--")); SetText(ConvertWidgets,TEXT("TXT_ConvertPercent"),FormatPercent(GR));
	const bool HasElapsed=GP.GenerationState!=ERouteWaypointBatchState::Idle||GP.TotalRoutes>0||GP.ElapsedSeconds>0; SetText(ConvertWidgets,TEXT("TXT_ArtifactElapsedValue"),HasElapsed?FormatElapsedMilliseconds(FMath::Max(0.0,GP.ElapsedSeconds)*1000.0):TEXT("--"));
	const UDSPGCSUploadJobSubsystem* Upload=GI->GetSubsystem<UDSPGCSUploadJobSubsystem>(); const dspgcs::UploadJobSnapshotDTO* Job=Upload?Upload->GetStateMirror().GetSnapshot():nullptr;
	const auto Count=[this,Job](FName Name,uint32 Value){GCSRouteWorkflowPrivate::SetText(UploadWidgets,Name,Job?FString::Printf(TEXT("%u"),Value):TEXT("0"));};
	Count(TEXT("TXT_UploadMatched"),Job?Job->matchedCount:0); Count(TEXT("TXT_UploadVacant"),Job?Job->vacantCount:0); Count(TEXT("TXT_UploadSpare"),Job?Job->spareCount:0); Count(TEXT("TXT_UploadWaiting"),Job?Job->waitingCount:0); Count(TEXT("TXT_UploadPreparing"),Job?Job->preparingCount:0); Count(TEXT("TXT_UploadUploading"),Job?Job->uploadingCount:0); Count(TEXT("TXT_UploadRetryWaiting"),Job?Job->retryWaitingCount:0); Count(TEXT("TXT_UploadSucceeded"),Job?Job->succeededCount:0); Count(TEXT("TXT_UploadFailed"),Job?Job->failedCount:0); Count(TEXT("TXT_UploadCancelled"),Job?Job->cancelledCount:0);
	SetText(UploadWidgets,TEXT("TXT_ConsoleValidatedValue"),Job?Gate(Job->artifactGateState):TEXT("--")); SetText(UploadWidgets,TEXT("TXT_UploadJobStateValue"),Job?TEXT("Protocol: ")+JobState(Job->jobState):TEXT("Protocol: --"));
	const uint64 Total=Job?Job->totalBytes:0,Done=Job?Job->bytesCompleted:0; const double UR=ComputeProgressRatio(Done,Total); SetText(UploadWidgets,TEXT("TXT_UploadThroughputValue"),FormatThroughput(Job?Job->throughputBytesPerSecond:0)); SetText(UploadWidgets,TEXT("TXT_UploadBytesValue"),FString::Printf(TEXT("%s / %s"),*FormatByteCount(Done),*FormatByteCount(Total))); SetText(UploadWidgets,TEXT("TXT_UploadElapsedValue"),FormatElapsedMilliseconds(Job?Job->elapsedMs:0)); if(UProgressBar* Bar=Get<UProgressBar>(UploadWidgets,TEXT("PROGRESS_Upload"))) Bar->SetPercent(UR); SetText(UploadWidgets,TEXT("TXT_UploadPercent"),FormatPercent(UR)); SetEdit(UploadWidgets,TEXT("EDT_MaxConcurrency"),Job?FString::Printf(TEXT("%u"),Job->effectiveMaxConcurrentUploads):TEXT("--"));
}

bool UGCSRouteWorkflowCoordinator::TickRefresh(float DeltaSeconds) { (void)DeltaSeconds; if(RouteWorkflowWidget&&RouteWorkflowWidget->IsVisible()) RefreshReadOnlyState(); return true; }
void UGCSRouteWorkflowCoordinator::StartRefreshTicker() { if(!RefreshTickerHandle.IsValid()) RefreshTickerHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this,&UGCSRouteWorkflowCoordinator::TickRefresh),RefreshIntervalSeconds); }
void UGCSRouteWorkflowCoordinator::StopRefreshTicker() { if(RefreshTickerHandle.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle); RefreshTickerHandle.Reset(); } }
double UGCSRouteWorkflowCoordinator::ComputeProgressRatio(uint64 Completed,uint64 Total) { return Total==0?0.0:FMath::Clamp(static_cast<double>(Completed)/Total,0.0,1.0); }
FString UGCSRouteWorkflowCoordinator::FormatByteCount(uint64 Bytes) { using namespace GCSRouteWorkflowPrivate; constexpr double KB=1024.0,MB=KB*1024.0,GB=MB*1024.0; if(Bytes<1024)return FString::Printf(TEXT("%llu B"),Bytes); if(Bytes<static_cast<uint64>(MB))return Compact(Bytes/KB)+TEXT(" KB"); if(Bytes<static_cast<uint64>(GB))return Compact(Bytes/MB)+TEXT(" MB"); return Compact(Bytes/GB)+TEXT(" GB"); }
FString UGCSRouteWorkflowCoordinator::FormatThroughput(uint64 Value) { return FormatByteCount(Value)+TEXT("/s"); }
FString UGCSRouteWorkflowCoordinator::FormatElapsedMilliseconds(uint64 Value) { const uint64 S=Value/1000; return FString::Printf(TEXT("%02llu:%02llu:%02llu"),S/3600,(S/60)%60,S%60); }
FString UGCSRouteWorkflowCoordinator::FormatPercent(double Value) { return FString::Printf(TEXT("%.0f%%"),FMath::Clamp(Value,0.0,1.0)*100.0); }

#if WITH_DEV_AUTOMATION_TESTS
void UGCSRouteWorkflowCoordinator::InitializeForTest(UGameInstance* GI) { Shutdown(); GameInstanceRef=GI; }
void UGCSRouteWorkflowCoordinator::SetWorkflowFactoryForTest(TFunction<UUserWidget*()> Factory,bool bSkip) { WorkflowFactoryForTest=MoveTemp(Factory); bSkipRequiredWidgetBindingForTest=bSkip; }
#endif
