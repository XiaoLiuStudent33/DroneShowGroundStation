#if WITH_DEV_AUTOMATION_TESTS

#include "GCSRouteWorkflowCoordinator.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "DSPGCSUploadJobProtocol.h"
#include "DSPGCSUploadJobSubsystem.h"
#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "DroneLogicalId.h"
#include "HAL/PlatformTime.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowOwnershipTest,"DroneGCS.UI.RouteWorkflow.OwnershipAndTeardown",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowOwnershipTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GI=NewObject<UGameInstance>(GEngine);
	if(!TestNotNull(TEXT("GameInstance"),GI)) return false;
	GI->InitializeStandalone(TEXT("GCSRouteWorkflowOwnershipTest"));
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>(GI);
	C->InitializeForTest(GI);
	UClass* WorkflowClass=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_RouteWorkflow.WBP_GCS_RouteWorkflow_C"));
	if(!TestNotNull(TEXT("Workflow class"),WorkflowClass)) { GI->Shutdown(); return false; }
	C->SetWorkflowFactoryForTest([GI,WorkflowClass](){return CreateWidget<UUserWidget>(GI,WorkflowClass);},false);
	FString Error;
	TestTrue(TEXT("First Show"),C->ShowRouteWorkflow(Error));
	UUserWidget* First=C->GetWorkflowWidgetForTest();
	TestNotNull(TEXT("Widget created"),First);
	UUserWidget* Upload=First&&First->WidgetTree?Cast<UUserWidget>(First->WidgetTree->FindWidget(TEXT("WBP_GCS_RouteUpload"))):nullptr;
	TestNotNull(TEXT("Nested RouteUpload"),Upload);
	for(const FName Name:{FName(TEXT("BTN_StartUpload")),FName(TEXT("BTN_PauseUpload")),FName(TEXT("BTN_ResumeUpload")),FName(TEXT("BTN_CancelUpload")),FName(TEXT("BTN_RetryFailed")),FName(TEXT("BTN_ApplyMaxConcurrency"))})
	{
		UButton* Button=Upload&&Upload->WidgetTree?Cast<UButton>(Upload->WidgetTree->FindWidget(Name)):nullptr;
		TestNotNull(*FString::Printf(TEXT("%s resolved"),*Name.ToString()),Button);
		if(Button) TestEqual(*FString::Printf(TEXT("%s initial state"),*Name.ToString()),
			Button->GetIsEnabled(),Name==FName(TEXT("BTN_ApplyMaxConcurrency")));
	}
	UEditableTextBox* Concurrency=Upload&&Upload->WidgetTree?Cast<UEditableTextBox>(Upload->WidgetTree->FindWidget(TEXT("EDT_MaxConcurrency"))):nullptr;
	TestNotNull(TEXT("Concurrency resolved"),Concurrency);
	if(Concurrency)
	{
		TestFalse(TEXT("Concurrency draft is editable"),Concurrency->GetIsReadOnly());
		TestTrue(TEXT("Concurrency draft is enabled"),Concurrency->GetIsEnabled());
		Concurrency->SetText(FText::FromString(TEXT("37")));
		C->RefreshReadOnlyStateForTest();
		TestEqual(TEXT("Snapshot refresh does not overwrite concurrency draft"),
			Concurrency->GetText().ToString(),FString(TEXT("37")));
		C->ClickApplyMaxConcurrencyForTest();
		TestEqual(TEXT("Apply stores operator concurrency"),
			C->GetAppliedMaxConcurrentTargetsForTest(),37);
		Concurrency->SetText(FText::FromString(TEXT("51")));
		AddExpectedError(TEXT("Maximum concurrency must be in range 1..50."),
			EAutomationExpectedErrorFlags::Contains,1);
		C->ClickApplyMaxConcurrencyForTest();
		TestEqual(TEXT("Invalid Apply preserves previous value"),
			C->GetAppliedMaxConcurrentTargetsForTest(),37);
	}
	UTextBlock* CurrentConcurrency=Upload&&Upload->WidgetTree?Cast<UTextBlock>(Upload->WidgetTree->FindWidget(TEXT("TXT_CurrentMaxConcurrencyValue"))):nullptr;
	TestNotNull(TEXT("Current concurrency presentation resolved"),CurrentConcurrency);
	C->RefreshReadOnlyStateForTest();
	if(CurrentConcurrency) TestEqual(TEXT("Applied concurrency presentation"),
		CurrentConcurrency->GetText().ToString(),FString(TEXT("Current: 37")));
	TestEqual(TEXT("Created once"),C->GetWorkflowCreationCountForTest(),1);
	TestTrue(TEXT("Ticker active"),C->IsRefreshTickerActiveForTest());
	C->ClickUploadTabForTest();
	TestEqual(TEXT("Upload tab selected"),C->GetSelectedPageIndexForTest(),1);
	C->ClickCloseForTest();
	TestFalse(TEXT("Close hides view"),C->IsViewVisibleForTest());
	TestEqual(TEXT("Close collapses retained widget"),First->GetVisibility(),ESlateVisibility::Collapsed);
	TestTrue(TEXT("Close keeps refresh ticker active"),C->IsRefreshTickerActiveForTest());
	TestTrue(TEXT("Closed Show"),C->ShowRouteWorkflow(Error));
	TestEqual(TEXT("Applied concurrency survives Close/Open"),
		C->GetAppliedMaxConcurrentTargetsForTest(),37);
	TestEqual(TEXT("Selected tab survives Close/Open"),C->GetSelectedPageIndexForTest(),1);
	TestTrue(TEXT("Repeated Show"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Repeated Show reuses"),C->GetWorkflowWidgetForTest()==First);
	TestEqual(TEXT("Still created once"),C->GetWorkflowCreationCountForTest(),1);
	UButton* CloseButton=First&&First->WidgetTree
		?Cast<UButton>(First->WidgetTree->FindWidget(TEXT("BTN_RouteClose"))):nullptr;
	TestNotNull(TEXT("Close button is C++ bound"),CloseButton);
	const int32 CloseCountBefore=C->GetCloseRequestCountForTest();
	if(CloseButton) CloseButton->OnClicked.Broadcast();
	TestEqual(TEXT("One click dispatches one C++ Close handler"),
		C->GetCloseRequestCountForTest(),CloseCountBefore+1);
	TestTrue(TEXT("Show after authored Close"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Repeated Show remains idempotent"),C->ShowRouteWorkflow(Error));
	if(CloseButton) CloseButton->OnClicked.Broadcast();
	TestEqual(TEXT("Reopen does not duplicate Close Delegate"),
		C->GetCloseRequestCountForTest(),CloseCountBefore+2);
	TestTrue(TEXT("Show after second authored Close"),C->ShowRouteWorkflow(Error));
	UWidget* AuthoredRoot=First&&First->WidgetTree
		?First->WidgetTree->FindWidget(TEXT("BORDER_RouteWorkflow")):nullptr;
	TestTrue(TEXT("Open restores authored root visibility"),
		AuthoredRoot&&AuthoredRoot->GetVisibility()==ESlateVisibility::Visible);
	First->SetVisibility(ESlateVisibility::Hidden);
	TestTrue(TEXT("Hidden Show"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Hidden Show reuses"),C->GetWorkflowWidgetForTest()==First);
	TestEqual(TEXT("Visible again"),First->GetVisibility(),ESlateVisibility::Visible);
	C->Shutdown();
	TestNull(TEXT("Reference cleared"),C->GetWorkflowWidgetForTest());
	TestFalse(TEXT("Ticker stopped"),C->IsRefreshTickerActiveForTest());
	GI->Shutdown();
	return true;
}

namespace GCSRouteWorkflowViewStateTest
{
	std::string ID(const char Value) { return std::string(32, Value); }

	dspgcs::UploadJobSnapshotDTO Snapshot(
		const std::string& JobID,
		const char Token,
		const dspgcs::UploadJobState State,
		const uint64 Revision,
		const uint64 CompletedBytes)
	{
		dspgcs::UploadJobSnapshotDTO Result;
		Result.uploadJobId = JobID;
		Result.snapshotToken = ID(Token);
		Result.jobState = State;
		Result.jobRevision = Revision;
		Result.snapshotSequence = Revision;
		Result.routeDeltaSequence = Revision;
		Result.matchedCount = 1;
		Result.totalBytes = 345800;
		Result.bytesCompleted = CompletedBytes;
		Result.throughputBytesPerSecond = CompletedBytes > 0 ? 39166 : 0;
		Result.elapsedMs = CompletedBytes > 0 ? 8829 : 0;
		Result.effectiveMaxConcurrentUploads = 1;
		switch (State)
		{
		case dspgcs::UploadJobState::Completed: Result.succeededCount = 1; break;
		case dspgcs::UploadJobState::Failed: Result.failedCount = 1; break;
		case dspgcs::UploadJobState::Cancelled: Result.cancelledCount = 1; break;
		case dspgcs::UploadJobState::Running: Result.uploadingCount = 1; break;
		default: Result.waitingCount = 1; break;
		}
		return Result;
	}

	dspgcs::UploadJobRouteStatePageDTO Page(
		const dspgcs::UploadJobSnapshotDTO& Snapshot)
	{
		dspgcs::UploadJobRouteStatePageDTO Result;
		Result.uploadJobId = Snapshot.uploadJobId;
		Result.snapshotToken = Snapshot.snapshotToken;
		Result.jobRevision = Snapshot.jobRevision;
		Result.totalRouteCount = 1;
		Result.pageStart = 0;
		Result.nextPageStart = 1;
		Result.hasMore = false;
		dspgcs::UploadRouteStateDTO Route;
		Route.routeSlotIndex = 0;
		Route.droneId = "Drone001";
		Route.totalBytes = Snapshot.totalBytes;
		Route.bytesCompleted = Snapshot.bytesCompleted;
		Route.attemptCount = 1;
		if (Snapshot.jobState == dspgcs::UploadJobState::Completed)
			Route.state = dspgcs::UploadRouteState::Succeeded;
		else if (Snapshot.jobState == dspgcs::UploadJobState::Failed)
			Route.state = dspgcs::UploadRouteState::Failed;
		else if (Snapshot.jobState == dspgcs::UploadJobState::Cancelled)
			Route.state = dspgcs::UploadRouteState::Cancelled;
		else Route.state = dspgcs::UploadRouteState::Uploading;
		Result.routes.push_back(Route);
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSRouteWorkflowViewStatePersistenceTest,
	"DroneGCS.UI.RouteWorkflow.ViewStatePersistence",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowViewStatePersistenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace GCSRouteWorkflowViewStateTest;
	UGameInstance* GI=NewObject<UGameInstance>(GEngine);
	if(!TestNotNull(TEXT("GameInstance"),GI)) return false;
	GI->InitializeStandalone(TEXT("GCSRouteWorkflowViewStatePersistenceTest"));
	UDSPGCSUploadJobSubsystem* Upload=GI->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>(GI);
	C->InitializeForTest(GI);
	UClass* WorkflowClass=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_RouteWorkflow.WBP_GCS_RouteWorkflow_C"));
	if(!TestNotNull(TEXT("Workflow class"),WorkflowClass)||!TestNotNull(TEXT("Upload subsystem"),Upload))
	{
		GI->Shutdown(); return false;
	}
	C->SetWorkflowFactoryForTest([GI,WorkflowClass](){return CreateWidget<UUserWidget>(GI,WorkflowClass);},false);
	FString Error;
	if(!TestTrue(TEXT("Show workflow"),C->ShowRouteWorkflow(Error)))
	{
		AddError(Error); C->Shutdown(); GI->Shutdown(); return false;
	}
	UUserWidget* Workflow=C->GetWorkflowWidgetForTest();
	UUserWidget* Convert=Workflow&&Workflow->WidgetTree
		?Cast<UUserWidget>(Workflow->WidgetTree->FindWidget(TEXT("WBP_GCS_RouteConvert"))):nullptr;
	UUserWidget* UploadPage=Workflow&&Workflow->WidgetTree
		?Cast<UUserWidget>(Workflow->WidgetTree->FindWidget(TEXT("WBP_GCS_RouteUpload"))):nullptr;
	if(!TestNotNull(TEXT("Convert page"),Convert)||!TestNotNull(TEXT("Upload page"),UploadPage))
	{
		C->Shutdown(); GI->Shutdown(); return false;
	}
	const auto Text=[UploadPage](const FName Name)
	{
		const UTextBlock* Value=UploadPage&&UploadPage->WidgetTree
			?Cast<UTextBlock>(UploadPage->WidgetTree->FindWidget(Name)):nullptr;
		return Value?Value->GetText().ToString():FString();
	};
	const auto BindState=[this,Upload,C](const char JobChar,const char Token,
		const dspgcs::UploadJobState State,const uint64 Revision,const uint64 Bytes)
	{
		const std::string JobID=ID(JobChar);
		Upload->BindUploadJobForTest(JobID);
		const auto Aggregate=Snapshot(JobID,Token,State,Revision,Bytes);
		TestTrue(TEXT("Aggregate accepted"),Upload->ApplySnapshotForTest(Aggregate));
		TestTrue(TEXT("Route page accepted"),Upload->ApplyRoutePageForTest(Page(Aggregate)));
		C->RefreshReadOnlyStateForTest();
	};
	const auto ClearTerminal=[this,Upload]()
	{
		FString ClearError;
		TestTrue(TEXT("Terminal Job may be cleared only explicitly"),
			Upload->PrepareForNewUploadJob(ClearError));
	};

	BindState('a','1',dspgcs::UploadJobState::Completed,10,345800);
	TestTrue(TEXT("Succeeded presented"),Text(TEXT("TXT_UploadJobStateValue")).Contains(TEXT("Succeeded")));
	TestTrue(TEXT("Succeeded logical bytes presented"),Text(TEXT("TXT_UploadBytesValue")).Contains(TEXT("345800 B / 345800 B")));
	TestEqual(TEXT("Succeeded Route row materialized"),C->GetRouteItemCountForTest(),1);
	C->ClickUploadTabForTest();
	C->ClickCloseForTest();
	TestTrue(TEXT("Succeeded close keeps authoritative snapshot"),Upload->GetStateMirror().HasAuthoritativeSnapshot());
	TestTrue(TEXT("Succeeded reopen"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Succeeded survives reopen"),Text(TEXT("TXT_UploadJobStateValue")).Contains(TEXT("Succeeded")));
	TestEqual(TEXT("Succeeded Route row survives reopen"),C->GetRouteItemCountForTest(),1);
	TestEqual(TEXT("Upload tab survives reopen"),C->GetSelectedPageIndexForTest(),1);
	ClearTerminal();

	BindState('b','2',dspgcs::UploadJobState::Failed,20,120000);
	C->ClickCloseForTest(); TestTrue(TEXT("Failed reopen"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Failed survives reopen"),Text(TEXT("TXT_UploadJobStateValue")).Contains(TEXT("Failed")));
	ClearTerminal();

	BindState('c','3',dspgcs::UploadJobState::Cancelled,30,80000);
	C->ClickCloseForTest(); TestTrue(TEXT("Cancelled reopen"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Cancelled survives reopen"),Text(TEXT("TXT_UploadJobStateValue")).Contains(TEXT("Cancelled")));

	FGCSRouteWorkflowCommandGateInputs BrowseGate;
	BrowseGate.bWorkspaceReady=true;
	C->SetCommandGateInputsForTest(BrowseGate);
	int32 PrepareCalls=0;
	FGCSRouteWorkflowCommandTestHooks Hooks;
	Hooks.OpenNpzDialog=[](FString& Out){Out=TEXT("C:/fixtures/persistent-route.npz");return true;};
	Hooks.PrepareRoute=[&PrepareCalls](const FString&,FString&){++PrepareCalls;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickRouteBrowseForTest();
	TestEqual(TEXT("Browse prepares candidate once"),PrepareCalls,1);
	const FString PreparedPath=C->GetSelectedRoutePathForTest();
	TestTrue(TEXT("Browse does not clear terminal Job"),Upload->GetStateMirror().HasAuthoritativeSnapshot());

	if(UEditableTextBox* Latitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginLatitude")))) Latitude->SetText(FText::FromString(TEXT("30.5")));
	if(UEditableTextBox* Longitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginLongitude")))) Longitude->SetText(FText::FromString(TEXT("120.25")));
	if(UEditableTextBox* Altitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginAltitude")))) Altitude->SetText(FText::FromString(TEXT("50")));
	FGCSRouteWorkflowCommandGateInputs LoadGate;
	LoadGate.bHasPreparedRoute=true; LoadGate.bOriginValid=true; LoadGate.bWorkspaceReady=true;
	C->SetCommandGateInputsForTest(LoadGate);
	int32 AtomicCommitCalls=0;
	Hooks={};
	Hooks.CommitRouteWorkflow=[&AtomicCommitCalls](double,double,double,bool,FString& OutError)
	{
		++AtomicCommitCalls; OutError=TEXT("Synthetic preview creation failure"); return false;
	};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	AddExpectedError(TEXT("Synthetic preview creation failure"),EAutomationExpectedErrorFlags::Contains,1);
	C->ClickLoadPreviewForTest();
	TestEqual(TEXT("Failed Load attempts candidate once"),AtomicCommitCalls,1);
	TestTrue(TEXT("Failed Load preserves old upload result"),Upload->GetStateMirror().HasAuthoritativeSnapshot());

	Hooks={};
	Hooks.CommitRouteWorkflow=[&AtomicCommitCalls](double,double,double,bool,FString&)
	{
		++AtomicCommitCalls; return true;
	};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickLoadPreviewForTest();
	TestFalse(TEXT("Successful Load clears prior upload result"),Upload->GetStateMirror().HasAuthoritativeSnapshot());
	TestFalse(TEXT("Successful Load clears prior active Job binding"),Upload->GetCommandStatus().bHasActiveJobBinding);

	BindState('d','4',dspgcs::UploadJobState::Completed,40,345800);
	C->ClickLoadPreviewForTest();
	TestFalse(TEXT("Same-file successful reload also clears prior result"),Upload->GetStateMirror().HasAuthoritativeSnapshot());
	TestEqual(TEXT("Same-file reload retains committed path"),C->GetSelectedRoutePathForTest(),PreparedPath);

	for(const dspgcs::UploadJobState ActiveState:{
		dspgcs::UploadJobState::Created,
		dspgcs::UploadJobState::Validating,
		dspgcs::UploadJobState::Ready,
		dspgcs::UploadJobState::Running,
		dspgcs::UploadJobState::Paused,
		dspgcs::UploadJobState::Cancelling})
	{
		UDSPGCSUploadJobSubsystem* StateProbe=NewObject<UDSPGCSUploadJobSubsystem>(GI);
		const std::string ProbeID=ID(static_cast<char>('g'+static_cast<int32>(ActiveState)));
		StateProbe->BindUploadJobForTest(ProbeID);
		TestTrue(TEXT("Active-state probe snapshot accepted"),
			StateProbe->ApplySnapshotForTest(Snapshot(ProbeID,'9',ActiveState,1,0)));
		FString ProbeError;
		TestFalse(TEXT("Every non-terminal aggregate blocks Dataset replacement"),
			StateProbe->CanPrepareForNewUploadJob(ProbeError));
	}

	BindState('e','5',dspgcs::UploadJobState::Running,50,54188);
	AtomicCommitCalls=0;
	Hooks={};
	Hooks.CommitRouteWorkflow=[&AtomicCommitCalls](double,double,double,bool,FString&){++AtomicCommitCalls;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	AddExpectedError(TEXT("当前仍有上传任务运行"),EAutomationExpectedErrorFlags::Contains,1);
	C->ClickLoadPreviewForTest();
	TestEqual(TEXT("Active Job rejects Load before Dataset commit"),AtomicCommitCalls,0);
	TestTrue(TEXT("Active Job remains bound after rejected Load"),Upload->GetCommandStatus().bHasActiveJobBinding);

	C->ClickCloseForTest();
	const int64 RevisionBeforeClose=Upload->GetMirrorStatus().JobRevision;
	auto Progress=Snapshot(ID('e'),'5',dspgcs::UploadJobState::Running,51,170000);
	TestTrue(TEXT("Running progress continues while View closed"),Upload->ApplySnapshotForTest(Progress));
	C->RefreshReadOnlyStateForTest();
	TestTrue(TEXT("Running reopen"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Reopen renders latest hidden progress"),Text(TEXT("TXT_UploadBytesValue")).Contains(TEXT("170000 B")));
	TestTrue(TEXT("Close preserves and advances authoritative revision"),
		RevisionBeforeClose==50&&Upload->GetMirrorStatus().JobRevision==51);

	C->ClickCloseForTest();
	auto Terminal=Snapshot(ID('e'),'5',dspgcs::UploadJobState::Completed,52,345800);
	TestTrue(TEXT("Job may become terminal while View closed"),Upload->ApplySnapshotForTest(Terminal));
	C->RefreshReadOnlyStateForTest();
	TestTrue(TEXT("Terminal reopen"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Closed-running Job reopens terminal"),Text(TEXT("TXT_UploadJobStateValue")).Contains(TEXT("Succeeded")));
	ClearTerminal();

	BindState('f','6',dspgcs::UploadJobState::Running,60,1000);
	const auto LateJobA=Snapshot(ID('e'),'5',dspgcs::UploadJobState::Completed,99,345800);
	TestFalse(TEXT("Late Job A snapshot cannot overwrite Job B"),Upload->ApplySnapshotForTest(LateJobA));
	const std::string JobBID=ID('f');
	TestTrue(TEXT("Job B remains authoritative"),
		Upload->GetMirrorStatus().UploadJobID==FString(UTF8_TO_TCHAR(JobBID.c_str())));

	const int32 CreationCount=C->GetWorkflowCreationCountForTest();
	for(int32 Index=0;Index<3;++Index)
	{
		C->ClickCloseForTest();
		TestTrue(TEXT("Repeated reopen succeeds"),C->ShowRouteWorkflow(Error));
	}
	TestEqual(TEXT("Repeated Close/Open never recreates Widget"),C->GetWorkflowCreationCountForTest(),CreationCount);
	TestTrue(TEXT("Repeated Close/Open keeps one refresh ticker"),C->IsRefreshTickerActiveForTest());
	C->ClickConvertTabForTest(); TestEqual(TEXT("Convert tab selection"),C->GetSelectedPageIndexForTest(),0);
	C->ClickUploadTabForTest(); TestEqual(TEXT("Upload tab selection"),C->GetSelectedPageIndexForTest(),1);

	C->Shutdown();
	TestNull(TEXT("Runtime teardown releases Widget"),C->GetWorkflowWidgetForTest());
	TestFalse(TEXT("Runtime teardown removes refresh Delegate"),C->IsRefreshTickerActiveForTest());
	GI->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowLookupFailureTest,"DroneGCS.UI.RouteWorkflow.RequiredWidgetLookupFailure",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowLookupFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GI=NewObject<UGameInstance>(GEngine);
	if(!TestNotNull(TEXT("GameInstance"),GI)) return false;
	GI->InitializeStandalone(TEXT("GCSRouteWorkflowLookupFailureTest"));
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>(GI);
	C->InitializeForTest(GI);
	UClass* MainClass=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
	if(!TestNotNull(TEXT("Non-workflow widget class"),MainClass)) { GI->Shutdown(); return false; }
	C->SetWorkflowFactoryForTest([GI,MainClass](){return CreateWidget<UUserWidget>(GI,MainClass);},false);
	FString Error;
	AddExpectedError(TEXT("missing/wrong required widgets"),EAutomationExpectedErrorFlags::Contains,1);
	TestFalse(TEXT("Missing hierarchy fails"),C->ShowRouteWorkflow(Error));
	TestFalse(TEXT("Diagnostic returned"),Error.IsEmpty());
	TestFalse(TEXT("Ticker not started"),C->IsRefreshTickerActiveForTest());
	TestFalse(TEXT("Binding invalid"),C->IsBindingValidForTest());
	C->Shutdown(); GI->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSRouteWorkflowConvertWithoutAssignmentWidgetsTest,
	"DroneGCS.UI.RouteWorkflow.ConvertWithoutAssignmentWidgets",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowConvertWithoutAssignmentWidgetsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GI=NewObject<UGameInstance>(GEngine);
	if(!TestNotNull(TEXT("GameInstance"),GI)) return false;
	GI->InitializeStandalone(TEXT("GCSRouteWorkflowConvertWithoutAssignmentWidgetsTest"));
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>(GI);
	C->InitializeForTest(GI);
	UClass* WorkflowClass=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_RouteWorkflow.WBP_GCS_RouteWorkflow_C"));
	if(!TestNotNull(TEXT("Workflow class"),WorkflowClass)) { GI->Shutdown(); return false; }

	const TArray<FName> AssignmentWidgetNames={
		TEXT("TXT_AssignmentState"),TEXT("TXT_AssignmentMode"),TEXT("TXT_ConsoleAccepted"),
		TEXT("TXT_MatchedCount"),TEXT("TXT_VacantCount"),TEXT("TXT_SpareCount"),
		TEXT("TXT_SectionAssignment"),TEXT("TXT_StateAssignmentLabel"),
		TEXT("TXT_StateAssignmentValue"),TEXT("HBOX_AssignmentState"),
		TEXT("HBOX_AssignmentCounts"),
		TEXT("HBOX_StateAssignment")};
	C->SetWorkflowFactoryForTest([GI,WorkflowClass,AssignmentWidgetNames]()
	{
		UUserWidget* Workflow=CreateWidget<UUserWidget>(GI,WorkflowClass);
		UUserWidget* Convert=Workflow&&Workflow->WidgetTree
			? Cast<UUserWidget>(Workflow->WidgetTree->FindWidget(TEXT("WBP_GCS_RouteConvert"))):nullptr;
		if(Convert&&Convert->WidgetTree)
		{
			for(const FName Name:AssignmentWidgetNames)
			{
				if(UWidget* Widget=Convert->WidgetTree->FindWidget(Name))
				{
					Convert->WidgetTree->RemoveWidget(Widget);
				}
			}
		}
		return Workflow;
	},false);

	FString Error;
	if(!TestTrue(TEXT("Open RouteWorkflow without Convert Assignment widgets"),C->ShowRouteWorkflow(Error)))
	{
		AddError(Error); C->Shutdown(); GI->Shutdown(); return false;
	}
	TestTrue(TEXT("Required binding remains valid"),C->IsBindingValidForTest());
	UUserWidget* Workflow=C->GetWorkflowWidgetForTest();
	UUserWidget* Convert=Workflow&&Workflow->WidgetTree
		? Cast<UUserWidget>(Workflow->WidgetTree->FindWidget(TEXT("WBP_GCS_RouteConvert"))):nullptr;
	if(!TestNotNull(TEXT("RouteConvert page"),Convert))
	{
		C->Shutdown(); GI->Shutdown(); return false;
	}
	for(const FName Name:AssignmentWidgetNames)
	{
		TestNull(*FString::Printf(TEXT("%s may be absent"),*Name.ToString()),
			Convert->WidgetTree->FindWidget(Name));
	}
	for(const FName Name:{FName(TEXT("HBOX_StateArtifact")),FName(TEXT("TXT_StateArtifactLabel")),
		FName(TEXT("TXT_StateArtifactValue"))})
	{
		TestNotNull(*FString::Printf(TEXT("Artifact widget %s remains bound"),*Name.ToString()),
			Convert->WidgetTree->FindWidget(Name));
	}

	FGCSRouteWorkflowCommandGateInputs BrowseGate;
	BrowseGate.bWorkspaceReady=true;
	C->SetCommandGateInputsForTest(BrowseGate);
	int32 PrepareCalls=0;
	FGCSRouteWorkflowCommandTestHooks Hooks;
	Hooks.OpenNpzDialog=[](FString& Out){Out=TEXT("C:/RouteCleanup/fixture.npz");return true;};
	Hooks.PrepareRoute=[&PrepareCalls](const FString&,FString&){++PrepareCalls;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickRouteBrowseForTest();
	TestEqual(TEXT("Browse/Prepare dispatch remains bound"),PrepareCalls,1);
	TestTrue(TEXT("Prepared path retained"),C->GetSelectedRoutePathForTest().EndsWith(TEXT("fixture.npz")));
	TestNotNull(TEXT("Dataset state presentation remains bound"),
		Convert->WidgetTree->FindWidget(TEXT("TXT_StateDatasetValue")));

	if(UEditableTextBox* Latitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginLatitude"))))
		Latitude->SetText(FText::FromString(TEXT("30.5")));
	if(UEditableTextBox* Longitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginLongitude"))))
		Longitude->SetText(FText::FromString(TEXT("120.25")));
	if(UEditableTextBox* Altitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginAltitude"))))
		Altitude->SetText(FText::FromString(TEXT("50")));
	FGCSRouteWorkflowCommandGateInputs LoadGate;
	LoadGate.bHasPreparedRoute=true; LoadGate.bOriginValid=true;
	C->SetCommandGateInputsForTest(LoadGate);
	int32 LoadSteps=0;
	Hooks={};
	Hooks.CommitRoute=[&LoadSteps](FString&){++LoadSteps;return true;};
	Hooks.SetOrigin=[&LoadSteps](double,double,double,FString&){++LoadSteps;return true;};
	Hooks.SetOverlay=[&LoadSteps](bool,FString&){++LoadSteps;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickLoadPreviewForTest();
	TestEqual(TEXT("Load Preview dispatch remains bound"),LoadSteps,3);

	FGCSRouteWorkflowCommandGateInputs GenerateGate;
	GenerateGate.bRouteLoaded=true; GenerateGate.bShowOriginSet=true;
	GenerateGate.bWorkspaceReady=true;
	C->SetCommandGateInputsForTest(GenerateGate);
	int32 GenerateCalls=0;
	Hooks={};
	Hooks.StartGeneration=[&GenerateCalls](int32,FString&){++GenerateCalls;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickGenerateWaypointsForTest();
	TestEqual(TEXT("Generate dispatch remains bound"),GenerateCalls,1);

	C->RefreshReadOnlyStateForTest();
	UTextBlock* ArtifactState=Cast<UTextBlock>(Convert->WidgetTree->FindWidget(TEXT("TXT_StateArtifactValue")));
	TestTrue(TEXT("Artifact state refresh remains bound"),ArtifactState&&!ArtifactState->GetText().IsEmpty());

	C->Shutdown(); GI->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowFormattingTest,"DroneGCS.UI.RouteWorkflow.ProgressAndFormatting",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowFormattingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("0/0"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(0,0),0.0);
	TestEqual(TEXT("0/100"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(0,100),0.0);
	TestEqual(TEXT("50/100"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(50,100),0.5);
	TestEqual(TEXT("100/100"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(100,100),1.0);
	TestEqual(TEXT("120/100 clamp"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(120,100),1.0);
	TestEqual(TEXT("500/1000"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(500,1000),0.5);
	TestEqual(TEXT("1000/1000"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(1000,1000),1.0);
	TestEqual(TEXT("bounded"),UGCSRouteWorkflowCoordinator::ComputeProgressRatio(1001,1000),1.0);
	TestEqual(TEXT("bytes 0"),UGCSRouteWorkflowCoordinator::FormatByteCount(0),FString(TEXT("0 B")));
	TestEqual(TEXT("bytes KB"),UGCSRouteWorkflowCoordinator::FormatByteCount(128*1024),FString(TEXT("128 KB")));
	TestEqual(TEXT("bytes MB"),UGCSRouteWorkflowCoordinator::FormatByteCount(2516582),FString(TEXT("2.4 MB")));
	TestEqual(TEXT("logical byte progress"),
		UGCSRouteWorkflowCoordinator::FormatLogicalByteProgress(345800,345800),
		FString(TEXT("345800 B / 345800 B (logical payload)")));
	TestEqual(TEXT("throughput"),UGCSRouteWorkflowCoordinator::FormatThroughput(125*1024),FString(TEXT("125 KB/s (avg logical payload)")));
	TestEqual(TEXT("elapsed"),UGCSRouteWorkflowCoordinator::FormatElapsedMilliseconds(92829),FString(TEXT("00:01:32.829")));
	TestEqual(TEXT("succeeded aggregate presentation"),
		UGCSRouteWorkflowCoordinator::FormatUploadJobPresentation(
			static_cast<uint8>(dspgcs::UploadJobState::Completed)),
		FString(TEXT("State: Succeeded")));
	TestEqual(TEXT("failed aggregate presentation"),
		UGCSRouteWorkflowCoordinator::FormatUploadJobPresentation(
			static_cast<uint8>(dspgcs::UploadJobState::Failed)),
		FString(TEXT("State: Failed")));
	TestEqual(TEXT("cancelled presentation never remains Running"),
		UGCSRouteWorkflowCoordinator::FormatUploadJobPresentation(
			static_cast<uint8>(dspgcs::UploadJobState::Cancelled)),
		FString(TEXT("State: Cancelled")));
	TestEqual(TEXT("0 percent"),UGCSRouteWorkflowCoordinator::FormatPercent(0),FString(TEXT("0%")));
	TestEqual(TEXT("50 percent"),UGCSRouteWorkflowCoordinator::FormatPercent(0.5),FString(TEXT("50%")));
	TestEqual(TEXT("100 percent"),UGCSRouteWorkflowCoordinator::FormatPercent(1),FString(TEXT("100%")));
	TestEqual(TEXT("retry attempt 0"),UGCSRouteWorkflowCoordinator::ComputeRetryCount(0),0);
	TestEqual(TEXT("retry attempt 1"),UGCSRouteWorkflowCoordinator::ComputeRetryCount(1),0);
	TestEqual(TEXT("retry attempt 3"),UGCSRouteWorkflowCoordinator::ComputeRetryCount(3),2);
	TestEqual(TEXT("manifest route basename"),
		UGCSRouteWorkflowCoordinator::FormatRouteFileDisplay(TEXT("000/42.waypoints")),
		FString(TEXT("42.waypoints")));
	const TArray<TPair<dspgcs::UploadRouteState,FString>> States = {
		{dspgcs::UploadRouteState::Waiting,TEXT("Waiting")},
		{dspgcs::UploadRouteState::Preparing,TEXT("Preparing")},
		{dspgcs::UploadRouteState::Uploading,TEXT("Protocol Uploading")},
		{dspgcs::UploadRouteState::RetryWaiting,TEXT("Retry Waiting")},
		{dspgcs::UploadRouteState::Succeeded,TEXT("Protocol Succeeded")},
		{dspgcs::UploadRouteState::Failed,TEXT("Failed")},
		{dspgcs::UploadRouteState::Cancelled,TEXT("Cancelled")}};
	for (const TPair<dspgcs::UploadRouteState,FString>& State : States)
	{
		TestEqual(*State.Value,
			UGCSRouteWorkflowCoordinator::FormatRouteState(static_cast<uint8>(State.Key)),
			State.Value);
	}
	TestEqual(TEXT("Route timeout presentation"),
		UGCSRouteWorkflowCoordinator::FormatRouteErrorCode(static_cast<uint16>(
			dspgcs::RouteUploadErrorCode::UploadTimedOut)),
		FString(TEXT("UploadTimedOut")));
	int32 Concurrency=0;
	FString Error;
	TestTrue(TEXT("Positive concurrency parses"),
		UGCSRouteWorkflowCoordinator::ParsePositiveConcurrency(
			TEXT(" 12 "),Concurrency,Error));
	TestEqual(TEXT("Positive concurrency value"),Concurrency,12);
	for(const FString& Invalid:{FString(),FString(TEXT("0")),FString(TEXT("-1")),
		FString(TEXT("1.5")),FString(TEXT("abc")),FString(TEXT("51")),
		FString(TEXT("2147483648")),FString(TEXT("999999999999999999999999"))})
	{
		TestFalse(*FString::Printf(TEXT("Invalid concurrency rejected: %s"),*Invalid),
			UGCSRouteWorkflowCoordinator::ParsePositiveConcurrency(
				Invalid,Concurrency,Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowUploadCommandGateTest,
	"DroneGCS.UI.RouteWorkflow.UploadCommandGate",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowUploadCommandGateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	auto NoneEnabled=[](const FGCSRouteUploadCommandGate& Gate)
	{
		return !Gate.bStartEnabled&&!Gate.bPauseEnabled&&!Gate.bResumeEnabled&&
			!Gate.bCancelEnabled&&!Gate.bRetryFailedEnabled&&!Gate.bApplyMaxEnabled;
	};
	FGCSRouteUploadCommandGateInputs Inputs;
	auto Gate=UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs);
	TestTrue(TEXT("No Artifact still permits operator Apply"),
		Gate.bApplyMaxEnabled&&!Gate.bStartEnabled&&!Gate.bPauseEnabled&&
		!Gate.bResumeEnabled&&!Gate.bCancelEnabled&&!Gate.bRetryFailedEnabled);
	Inputs.bArtifactReady=true;
	Gate=UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs);
	TestTrue(TEXT("No mapping still permits operator Apply"),
		Gate.bApplyMaxEnabled&&!Gate.bStartEnabled);
	Inputs.bAutoStartMappingValid=true;
	Gate=UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs);
	TestTrue(TEXT("Ready Artifact enables Start and pre-transaction Apply"),
		Gate.bStartEnabled&&!Gate.bPauseEnabled&&!Gate.bResumeEnabled&&
		!Gate.bCancelEnabled&&!Gate.bRetryFailedEnabled&&Gate.bApplyMaxEnabled);
	Inputs.bCommandInFlight=true;
	TestTrue(TEXT("Command in-flight disables all"),NoneEnabled(
		UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs)));
	Inputs.bCommandInFlight=false;
	Inputs.bAutoStartInFlight=true;
	TestTrue(TEXT("AutoStart in-flight disables all"),NoneEnabled(
		UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs)));
	Inputs.bAutoStartInFlight=false;
	Inputs.bHasActiveJobBinding=true;
	Inputs.bHasExecutableJob=true;
	Inputs.JobState=static_cast<uint8>(dspgcs::UploadJobState::Validating);
	TestTrue(TEXT("Validating disables all"),NoneEnabled(
		UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs)));
	for(const dspgcs::UploadJobState State:{dspgcs::UploadJobState::Ready,
		dspgcs::UploadJobState::Running})
	{
		Inputs.JobState=static_cast<uint8>(State);
		Gate=UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs);
		TestTrue(TEXT("Ready/Running gate"),Gate.bPauseEnabled&&Gate.bCancelEnabled&&
			!Gate.bApplyMaxEnabled&&!Gate.bStartEnabled&&!Gate.bResumeEnabled&&
			!Gate.bRetryFailedEnabled);
	}
	Inputs.JobState=static_cast<uint8>(dspgcs::UploadJobState::Paused);
	Gate=UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs);
	TestTrue(TEXT("Paused gate"),Gate.bResumeEnabled&&Gate.bCancelEnabled&&
		!Gate.bApplyMaxEnabled&&!Gate.bPauseEnabled&&!Gate.bStartEnabled);
	Inputs.JobState=static_cast<uint8>(dspgcs::UploadJobState::Failed);
	Inputs.FailedCount=0;
	Gate=UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs);
	TestTrue(TEXT("Failed without failed routes"),Gate.bCancelEnabled&&
		Gate.bApplyMaxEnabled&&!Gate.bRetryFailedEnabled);
	Inputs.FailedCount=2;
	TestTrue(TEXT("Failed routes enable RetryFailed"),
		UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs).bRetryFailedEnabled);
	for(const dspgcs::UploadJobState State:{dspgcs::UploadJobState::Completed,
		dspgcs::UploadJobState::Cancelled})
	{
		Inputs.JobState=static_cast<uint8>(State);
		Gate=UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs);
		TestTrue(TEXT("Terminal state permits next-Job Apply only"),
			Gate.bApplyMaxEnabled&&!Gate.bStartEnabled&&!Gate.bPauseEnabled&&
			!Gate.bResumeEnabled&&!Gate.bCancelEnabled&&!Gate.bRetryFailedEnabled);
	}
	Inputs.bHasExecutableJob=false;
	Inputs.JobState=0;
	TestTrue(TEXT("Bound validation error requires explicit reset"),NoneEnabled(
		UGCSRouteWorkflowCoordinator::ComputeUploadCommandGate(Inputs)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowArtifactIdentityTest,
	"DroneGCS.UI.RouteWorkflow.ArtifactIdentitySafety",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowArtifactIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString ArtifactA=FString::ChrN(64,TEXT('a'));
	const FString ArtifactB=FString::ChrN(64,TEXT('b'));
	const FString DigestA=FString::ChrN(64,TEXT('1'));
	const FString DigestB=FString::ChrN(64,TEXT('2'));
	const FString Root=TEXT("C:/Artifacts/Current");
	const FString SourceA=TEXT("Artifact-A.npz");

	FGCSRouteUploadArtifactDisplay Display=UGCSRouteWorkflowCoordinator::ResolveArtifactDisplay(
		false,TEXT(""),TEXT(""),true,ArtifactA,DigestA,Root,SourceA);
	TestEqual(TEXT("No Job uses Current ID"),Display.ArtifactSetID,ArtifactA);
	TestEqual(TEXT("No Job uses Current root"),Display.WaypointsRoot,Root);
	TestEqual(TEXT("Source comes from manifest identity"),Display.SourceDataset,SourceA);
	TestFalse(TEXT("No Job Current is not mismatch"),Display.bArtifactMismatch);

	Display=UGCSRouteWorkflowCoordinator::ResolveArtifactDisplay(
		true,ArtifactA,DigestA,true,ArtifactA,DigestA,Root,SourceA);
	TestEqual(TEXT("Matching Job keeps full authoritative ID"),Display.ArtifactSetID,ArtifactA);
	TestEqual(TEXT("Matching Job exposes Current root"),Display.WaypointsRoot,Root);
	TestEqual(TEXT("Matching Job exposes manifest source"),Display.SourceDataset,SourceA);
	TestFalse(TEXT("Matching Job is not mismatch"),Display.bArtifactMismatch);

	Display=UGCSRouteWorkflowCoordinator::ResolveArtifactDisplay(
		true,ArtifactA,DigestA,true,ArtifactB,DigestB,TEXT("C:/NewCurrent"),TEXT("Preview-B.npz"));
	TestEqual(TEXT("Mismatch preserves Job authoritative ID"),Display.ArtifactSetID,ArtifactA);
	TestTrue(TEXT("Mismatch is explicit"),Display.bArtifactMismatch);
	TestTrue(TEXT("Mismatch hides new Current root"),Display.WaypointsRoot.IsEmpty());
	TestTrue(TEXT("Mismatch hides unrelated source"),Display.SourceDataset.IsEmpty());

	Display=UGCSRouteWorkflowCoordinator::ResolveArtifactDisplay(
		true,ArtifactA,DigestA,false,TEXT(""),TEXT(""),TEXT(""),TEXT(""));
	TestTrue(TEXT("Job without Current is safe mismatch"),Display.bArtifactMismatch);
	TestEqual(TEXT("Job without Current preserves ID"),Display.ArtifactSetID,ArtifactA);
	TestTrue(TEXT("Job without Current hides root"),Display.WaypointsRoot.IsEmpty());

	Display=UGCSRouteWorkflowCoordinator::ResolveArtifactDisplay(
		false,TEXT(""),TEXT(""),false,ArtifactA,DigestA,Root,SourceA);
	TestTrue(TEXT("Invalid Current does not display ID"),Display.ArtifactSetID.IsEmpty());
	TestTrue(TEXT("Invalid Current does not display root"),Display.WaypointsRoot.IsEmpty());
	TestTrue(TEXT("Invalid Current does not display source"),Display.SourceDataset.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowRouteListScaleTest,
	"DroneGCS.UI.RouteWorkflow.RouteListScale",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowRouteListScaleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GI=NewObject<UGameInstance>(GEngine);
	if(!TestNotNull(TEXT("GameInstance"),GI)) return false;
	GI->InitializeStandalone(TEXT("GCSRouteWorkflowRouteListScaleTest"));
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>(GI);
	C->InitializeForTest(GI);
	UClass* WorkflowClass=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_RouteWorkflow.WBP_GCS_RouteWorkflow_C"));
	if(!TestNotNull(TEXT("Workflow class"),WorkflowClass)) { GI->Shutdown(); return false; }
	C->SetWorkflowFactoryForTest([GI,WorkflowClass](){return CreateWidget<UUserWidget>(GI,WorkflowClass);},false);
	FString Error;
	if(!TestTrue(TEXT("Workflow binds UI-3A widgets"),C->ShowRouteWorkflow(Error)))
	{
		AddError(Error); C->Shutdown(); GI->Shutdown(); return false;
	}
	UUserWidget* Workflow=C->GetWorkflowWidgetForTest();
	UUserWidget* Upload=Workflow&&Workflow->WidgetTree
		? Cast<UUserWidget>(Workflow->WidgetTree->FindWidget(TEXT("WBP_GCS_RouteUpload"))) : nullptr;
	UListView* List=Upload&&Upload->WidgetTree
		? Cast<UListView>(Upload->WidgetTree->FindWidget(TEXT("LIST_RouteUploadStates"))) : nullptr;
	TestNotNull(TEXT("Virtualized ListView resolved"),List);
	UClass* RowClass=List?List->GetEntryWidgetClass():nullptr;
	TestNotNull(TEXT("Route row entry class configured"),RowClass);
	if(RowClass)
	{
		TestTrue(TEXT("Route row implements IUserObjectListEntry"),
			RowClass->ImplementsInterface(UUserObjectListEntry::StaticClass()));
		UUserWidget* RowProbe=CreateWidget<UUserWidget>(GI,RowClass);
		TestNotNull(TEXT("Route row runtime probe"),RowProbe);
		for(const FName Name:{FName(TEXT("TXT_Route")),FName(TEXT("TXT_Drone")),
			FName(TEXT("TXT_State")),FName(TEXT("TXT_Progress")),FName(TEXT("TXT_Retry"))})
		{
			TestNotNull(*FString::Printf(TEXT("Route row %s resolved"),*Name.ToString()),
				RowProbe&&RowProbe->WidgetTree
					? Cast<UTextBlock>(RowProbe->WidgetTree->FindWidget(Name)):nullptr);
		}
	}

	for(const int32 Count:{1000,10000,20000})
	{
		TArray<int32> Slots; Slots.Reserve(Count);
		for(int32 Index=0;Index<Count;++Index) Slots.Add(Index);
		const FString JobID=FString::Printf(TEXT("job-%d"),Count);
		const FString Token=FString::Printf(TEXT("snapshot-%d"),Count);
		const int64 BeforeRebuild=C->GetRouteListRebuildCountForTest();
		const double Start=FPlatformTime::Seconds();
		TestTrue(*FString::Printf(TEXT("%d item build"),Count),C->BuildRouteItemsForTest(JobID,Token,Slots));
		const double BuildMs=(FPlatformTime::Seconds()-Start)*1000.0;
		TestEqual(*FString::Printf(TEXT("%d item count"),Count),C->GetRouteItemCountForTest(),Count);
		TestEqual(*FString::Printf(TEXT("%d one rebuild"),Count),C->GetRouteListRebuildCountForTest(),BeforeRebuild+1);
		const UGCSRouteUploadListItem* First=C->FindRouteItemForTest(0);
		const UGCSRouteUploadListItem* Middle=C->FindRouteItemForTest(Count/2);
		const UGCSRouteUploadListItem* Last=C->FindRouteItemForTest(Count-1);
		TestNotNull(TEXT("O(1) first lookup"),First);
		TestNotNull(TEXT("O(1) middle lookup"),Middle);
		TestNotNull(TEXT("O(1) last lookup"),Last);
		TestTrue(TEXT("same Job/snapshot refresh accepted"),C->BuildRouteItemsForTest(JobID,Token,Slots));
		TestTrue(TEXT("same Job keeps first item identity"),C->FindRouteItemForTest(0)==First);
		TestTrue(TEXT("same Job keeps middle item identity"),C->FindRouteItemForTest(Count/2)==Middle);
		TestEqual(TEXT("same Job does not rebuild"),C->GetRouteListRebuildCountForTest(),BeforeRebuild+1);
		const int32 Visible=List?List->GetDisplayedEntryWidgets().Num():0;
		const double RefreshStart=FPlatformTime::Seconds();
		C->RefreshVisibleRouteRowsForTest();
		const double RefreshMs=(FPlatformTime::Seconds()-RefreshStart)*1000.0;
		AddInfo(FString::Printf(TEXT("UI3A_SCALE count=%d build_ms=%.3f estimated_model_bytes=%llu visible_rows=%d visible_refresh_ms=%.3f"),
			Count,BuildMs,C->EstimateRouteItemModelBytes(Count),Visible,RefreshMs));
		if(Count==20000) TestTrue(TEXT("20K ListView does not instantiate 20K rows"),Visible<Count);
	}
	C->Shutdown(); GI->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowRouteRowContractTest,
	"DroneGCS.UI.RouteWorkflow.RouteRowContract",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowRouteRowContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GI=NewObject<UGameInstance>(GEngine);
	if(!TestNotNull(TEXT("GameInstance"),GI)) return false;
	GI->InitializeStandalone(TEXT("GCSRouteWorkflowRouteRowContractTest"));
	UClass* RowClass=LoadClass<UUserWidget>(nullptr,
		TEXT("/Game/UI/WBP_GCS_RouteUploadRow.WBP_GCS_RouteUploadRow_C"));
	UUserWidget* BlueprintRow=RowClass?CreateWidget<UUserWidget>(GI,RowClass):nullptr;
	TestNotNull(TEXT("Blueprint Route row"),BlueprintRow);
	if(BlueprintRow)
	{
		TestTrue(TEXT("Blueprint-only interface is implemented"),
			BlueprintRow->Implements<UUserObjectListEntry>());
		TestNull(TEXT("Blueprint-only interface has no required native pointer"),
			Cast<IUserObjectListEntry>(BlueprintRow));
		TestTrue(TEXT("Five production TextBlocks satisfy Row structure"),
			UGCSRouteWorkflowCoordinator::DiagnoseRouteRowStructure(BlueprintRow).IsEmpty());
	}

	UClass* NonRowClass=LoadClass<UUserWidget>(nullptr,
		TEXT("/Game/UI/WBP_GCS_RouteUpload.WBP_GCS_RouteUpload_C"));
	UUserWidget* InvalidRow=NonRowClass?CreateWidget<UUserWidget>(GI,NonRowClass):nullptr;
	if(!TestNotNull(TEXT("Non-row widget fixture"),InvalidRow))
	{
		GI->Shutdown();
		return false;
	}
	const FString MissingDiagnostic=
		UGCSRouteWorkflowCoordinator::DiagnoseRouteRowStructure(InvalidRow);
	TestTrue(TEXT("Interface missing is explicit"),
		MissingDiagnostic.Contains(TEXT("InterfaceMissing")));
	TestTrue(TEXT("Widget missing is explicit"),
		MissingDiagnostic.Contains(TEXT("TXT_Route Missing")));

	UButton* WrongRoute=InvalidRow->WidgetTree->ConstructWidget<UButton>(
		UButton::StaticClass(),TEXT("TXT_Route"));
	InvalidRow->WidgetTree->RootWidget=WrongRoute;
	const FString WrongTypeDiagnostic=
		UGCSRouteWorkflowCoordinator::DiagnoseRouteRowStructure(InvalidRow);
	TestTrue(TEXT("Widget wrong type is explicit"),
		WrongTypeDiagnostic.Contains(TEXT("TXT_Route WrongType")));
	GI->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowCommandOriginGateTest,
	"DroneGCS.UI.RouteWorkflow.CommandOriginAndGate",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowCommandOriginGateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	double Latitude=0.0,Longitude=0.0,Altitude=0.0;
	FString Error;
	TestTrue(TEXT("Valid origin"),UGCSRouteWorkflowCoordinator::ParseOriginInputs(
		TEXT(" 30.500000 "),TEXT("120.25"),TEXT("12.75"),
		Latitude,Longitude,Altitude,Error));
	TestEqual(TEXT("Parsed latitude"),Latitude,30.5);
	TestEqual(TEXT("Parsed longitude"),Longitude,120.25);
	TestEqual(TEXT("Parsed altitude"),Altitude,12.75);
	TestFalse(TEXT("Empty latitude rejected"),UGCSRouteWorkflowCoordinator::ParseOriginInputs(
		TEXT(""),TEXT("120"),TEXT("10"),Latitude,Longitude,Altitude,Error));
	TestFalse(TEXT("NaN rejected"),UGCSRouteWorkflowCoordinator::ParseOriginInputs(
		TEXT("NaN"),TEXT("120"),TEXT("10"),Latitude,Longitude,Altitude,Error));
	TestFalse(TEXT("Inf rejected"),UGCSRouteWorkflowCoordinator::ParseOriginInputs(
		TEXT("30"),TEXT("Inf"),TEXT("10"),Latitude,Longitude,Altitude,Error));
	TestFalse(TEXT("Partial parse rejected"),UGCSRouteWorkflowCoordinator::ParseOriginInputs(
		TEXT("30x"),TEXT("120"),TEXT("10"),Latitude,Longitude,Altitude,Error));
	TestFalse(TEXT("Latitude range rejected"),UGCSRouteWorkflowCoordinator::ParseOriginInputs(
		TEXT("90.01"),TEXT("120"),TEXT("10"),Latitude,Longitude,Altitude,Error));
	TestFalse(TEXT("Longitude range rejected"),UGCSRouteWorkflowCoordinator::ParseOriginInputs(
		TEXT("30"),TEXT("180.01"),TEXT("10"),Latitude,Longitude,Altitude,Error));

	FGCSRouteWorkflowCommandGateInputs Inputs;
	FGCSRouteWorkflowCommandGate Gate=UGCSRouteWorkflowCoordinator::ComputeCommandGate(Inputs);
	TestTrue(TEXT("No dataset allows Route Browse"),Gate.bRouteBrowseEnabled);
	TestTrue(TEXT("No dataset allows Output Browse"),Gate.bOutputBrowseEnabled);
	TestFalse(TEXT("No dataset blocks Load"),Gate.bLoadPreviewEnabled);
	TestFalse(TEXT("No dataset blocks Generate"),Gate.bGenerateWaypointsEnabled);
	Inputs.bHasPreparedRoute=true; Inputs.bOriginValid=true;
	Gate=UGCSRouteWorkflowCoordinator::ComputeCommandGate(Inputs);
	TestTrue(TEXT("Prepared valid origin allows Load"),Gate.bLoadPreviewEnabled);
	TestFalse(TEXT("Prepared blocks Generate"),Gate.bGenerateWaypointsEnabled);
	Inputs=FGCSRouteWorkflowCommandGateInputs{};
	Inputs.bRouteLoaded=true; Inputs.bShowOriginSet=true; Inputs.bWorkspaceReady=true;
	Gate=UGCSRouteWorkflowCoordinator::ComputeCommandGate(Inputs);
	TestTrue(TEXT("Offline route prerequisites allow Generate"),Gate.bGenerateWaypointsEnabled);
	Inputs.bGenerationActive=true;
	Gate=UGCSRouteWorkflowCoordinator::ComputeCommandGate(Inputs);
	TestFalse(TEXT("Generation active disables Route Browse"),Gate.bRouteBrowseEnabled);
	TestFalse(TEXT("Generation active disables Output Browse"),Gate.bOutputBrowseEnabled);
	TestFalse(TEXT("Generation active disables Load"),Gate.bLoadPreviewEnabled);
	TestFalse(TEXT("Generation active disables Generate"),Gate.bGenerateWaypointsEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowCommandDispatchTest,
	"DroneGCS.UI.RouteWorkflow.CommandDispatch",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowCommandDispatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GI=NewObject<UGameInstance>(GEngine);
	if(!TestNotNull(TEXT("GameInstance"),GI)) return false;
	GI->InitializeStandalone(TEXT("GCSRouteWorkflowCommandDispatchTest"));
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>(GI);
	C->InitializeForTest(GI);
	UClass* WorkflowClass=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_RouteWorkflow.WBP_GCS_RouteWorkflow_C"));
	if(!TestNotNull(TEXT("Workflow class"),WorkflowClass)) { GI->Shutdown(); return false; }
	C->SetWorkflowFactoryForTest([GI,WorkflowClass](){return CreateWidget<UUserWidget>(GI,WorkflowClass);},false);
	FString ShowError;
	if(!TestTrue(TEXT("Workflow command widgets bind"),C->ShowRouteWorkflow(ShowError)))
	{
		AddError(ShowError); C->Shutdown(); GI->Shutdown(); return false;
	}
	UUserWidget* Workflow=C->GetWorkflowWidgetForTest();
	UUserWidget* Convert=Workflow&&Workflow->WidgetTree
		? Cast<UUserWidget>(Workflow->WidgetTree->FindWidget(TEXT("WBP_GCS_RouteConvert"))):nullptr;
	if(!TestNotNull(TEXT("RouteConvert page"),Convert))
	{
		C->Shutdown(); GI->Shutdown(); return false;
	}
	UEditableTextBox* Latitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginLatitude")));
	UEditableTextBox* Longitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginLongitude")));
	UEditableTextBox* Altitude=Cast<UEditableTextBox>(Convert->WidgetTree->FindWidget(TEXT("EDT_OriginAltitude")));
	TestNotNull(TEXT("Latitude input"),Latitude); TestNotNull(TEXT("Longitude input"),Longitude); TestNotNull(TEXT("Altitude input"),Altitude);

	FGCSRouteWorkflowCommandGateInputs BrowseGate;
	BrowseGate.bWorkspaceReady=true;
	C->SetCommandGateInputsForTest(BrowseGate);
	int32 DialogCalls=0,PrepareCalls=0;
	FGCSRouteWorkflowCommandTestHooks Hooks;
	Hooks.OpenNpzDialog=[&DialogCalls](FString&){++DialogCalls;return false;};
	Hooks.PrepareRoute=[&PrepareCalls](const FString&,FString&){++PrepareCalls;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickRouteBrowseForTest();
	TestEqual(TEXT("NPZ cancel opens once"),DialogCalls,1);
	TestEqual(TEXT("NPZ cancel does not Prepare"),PrepareCalls,0);
	TestTrue(TEXT("NPZ cancel keeps selection empty"),C->GetSelectedRoutePathForTest().IsEmpty());

	Hooks={};
	Hooks.OpenNpzDialog=[&DialogCalls,C](FString& Out){++DialogCalls;Out=TEXT("C:/fixtures/show-a.npz");C->ClickRouteBrowseForTest();return true;};
	Hooks.PrepareRoute=[&PrepareCalls](const FString&,FString&){++PrepareCalls;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickRouteBrowseForTest();
	TestEqual(TEXT("Reentrant dialog call suppressed"),DialogCalls,2);
	TestEqual(TEXT("Valid NPZ Prepare called once"),PrepareCalls,1);
	const FString SuccessfulSelection=C->GetSelectedRoutePathForTest();
	TestTrue(TEXT("Successful NPZ path retained"),SuccessfulSelection.EndsWith(TEXT("show-a.npz")));

	FString ActiveRouteIdentity=TEXT("active-before-failure");
	Hooks={};
	Hooks.OpenNpzDialog=[](FString& Out){Out=TEXT("C:/fixtures/broken.npz");return true;};
	Hooks.PrepareRoute=[&PrepareCalls,&ActiveRouteIdentity](const FString&,FString& OutError){++PrepareCalls;OutError=TEXT("Synthetic prepare failure");return false;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	AddExpectedError(TEXT("Route Workflow command rejected: Synthetic prepare failure"),EAutomationExpectedErrorFlags::Contains,1);
	C->ClickRouteBrowseForTest();
	TestEqual(TEXT("Prepare failure preserves selected path"),C->GetSelectedRoutePathForTest(),SuccessfulSelection);
	TestEqual(TEXT("Prepare failure preserves active route"),ActiveRouteIdentity,FString(TEXT("active-before-failure")));
	TestTrue(TEXT("Command error has bounded expiry"),C->GetCommandErrorExpiryForTest()-FPlatformTime::Seconds()>0.0&&C->GetCommandErrorExpiryForTest()-FPlatformTime::Seconds()<=5.1);
	C->RefreshReadOnlyStateForTest();
	UTextBlock* DatasetState=Cast<UTextBlock>(Convert->WidgetTree->FindWidget(TEXT("TXT_StateDatasetValue")));
	TestTrue(TEXT("5Hz refresh preserves transient command error"),DatasetState&&DatasetState->GetText().ToString().Contains(TEXT("Synthetic prepare failure")));

	if(Latitude) Latitude->SetText(FText::FromString(TEXT("30.5")));
	if(Longitude) Longitude->SetText(FText::FromString(TEXT("120.25")));
	if(Altitude) Altitude->SetText(FText::FromString(TEXT("50")));
	FGCSRouteWorkflowCommandGateInputs LoadGate;
	LoadGate.bHasPreparedRoute=true; LoadGate.bOriginValid=true; LoadGate.bWorkspaceReady=true;
	C->SetCommandGateInputsForTest(LoadGate);
	TArray<FString> Sequence;
	Hooks={};
	Hooks.CommitRoute=[&Sequence](FString&){Sequence.Add(TEXT("Commit"));return true;};
	Hooks.SetOrigin=[&Sequence](double Lat,double Lon,double Alt,FString&){Sequence.Add(FString::Printf(TEXT("Origin:%.2f:%.2f:%.2f"),Lat,Lon,Alt));return true;};
	Hooks.SetOverlay=[&Sequence](bool bEnabled,FString&){Sequence.Add(bEnabled?TEXT("OverlayOn"):TEXT("OverlayOff"));return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickLoadPreviewForTest();
	TestEqual(TEXT("Load sequence count"),Sequence.Num(),3);
	if(Sequence.Num()==3)
	{
		TestEqual(TEXT("Load first Commit"),Sequence[0],FString(TEXT("Commit")));
		TestTrue(TEXT("Load second Origin"),Sequence[1].StartsWith(TEXT("Origin:")));
		TestEqual(TEXT("Load third Overlay"),Sequence[2],FString(TEXT("OverlayOn")));
	}

	Sequence.Reset();
	Hooks={};
	Hooks.CommitRoute=[&Sequence](FString&){Sequence.Add(TEXT("Commit"));return true;};
	Hooks.SetOrigin=[&Sequence](double,double,double,FString& OutError){Sequence.Add(TEXT("OriginFail"));OutError=TEXT("Synthetic origin failure");return false;};
	Hooks.SetOverlay=[&Sequence](bool,FString&){Sequence.Add(TEXT("UnexpectedOverlay"));return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	AddExpectedError(TEXT("Route Workflow command rejected: Synthetic origin failure"),EAutomationExpectedErrorFlags::Contains,1);
	C->ClickLoadPreviewForTest();
	TestEqual(TEXT("Partial failure stops before Overlay"),Sequence.Num(),2);

	C->SetCommandGateInputsForTest(BrowseGate);
	int32 FolderCalls=0,SetRootCalls=0;
	Hooks={};
	Hooks.OpenFolderDialog=[&FolderCalls](FString&){++FolderCalls;return false;};
	Hooks.SetWorkspaceRoot=[&SetRootCalls](const FString&,FString&){++SetRootCalls;return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickOutputBrowseForTest();
	TestEqual(TEXT("Folder cancel opens once"),FolderCalls,1);
	TestEqual(TEXT("Folder cancel does not set root"),SetRootCalls,0);

	FString ConfiguredRoot;
	Hooks={};
	Hooks.OpenFolderDialog=[&FolderCalls](FString& Out){++FolderCalls;Out=TEXT("C:/ArtifactsRoot");return true;};
	Hooks.SetWorkspaceRoot=[&SetRootCalls,&ConfiguredRoot](const FString& Root,FString&){++SetRootCalls;ConfiguredRoot=Root;return true;};
	Hooks.ResolveWorkspaceRoot=[](FString& Out,FString&){Out=TEXT("C:/ArtifactsRoot");return true;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickOutputBrowseForTest();
	TestEqual(TEXT("Workspace setter invoked once"),SetRootCalls,1);
	TestEqual(TEXT("Workspace root retained by authority hook"),ConfiguredRoot,FString(TEXT("C:/ArtifactsRoot")));

	Hooks={};
	Hooks.OpenFolderDialog=[](FString& Out){Out=TEXT("C:/RejectedRoot");return true;};
	Hooks.SetWorkspaceRoot=[&SetRootCalls](const FString&,FString& OutError){++SetRootCalls;OutError=TEXT("Synthetic workspace failure");return false;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	AddExpectedError(TEXT("Route Workflow command rejected: Synthetic workspace failure"),EAutomationExpectedErrorFlags::Contains,1);
	C->ClickOutputBrowseForTest();
	TestEqual(TEXT("Workspace failure preserves previous root"),ConfiguredRoot,FString(TEXT("C:/ArtifactsRoot")));

	FGCSRouteWorkflowCommandGateInputs ReadyGate;
	ReadyGate.bRouteLoaded=true; ReadyGate.bShowOriginSet=true;
	ReadyGate.bWorkspaceReady=true;
	C->SetCommandGateInputsForTest(ReadyGate);
	int32 StartCalls=0;
	Hooks={};
	Hooks.StartGeneration=[&StartCalls,C](int32 Workers,FString&){++StartCalls;C->ClickGenerateWaypointsForTest();return Workers==0;};
	C->SetCommandTestHooks(MoveTemp(Hooks));
	C->ClickGenerateWaypointsForTest();
	TestEqual(TEXT("StartGeneration(0) invoked once despite reentrancy"),StartCalls,1);

	ReadyGate.bGenerationActive=true;
	C->SetCommandGateInputsForTest(ReadyGate);
	C->RefreshCommandControlsForTest();
	for(const FName Name:{FName(TEXT("BTN_RouteBrowse")),FName(TEXT("BTN_OutputBrowse")),FName(TEXT("BTN_LoadPreview")),FName(TEXT("BTN_GenerateWaypoints"))})
	{
		UButton* Button=Cast<UButton>(Convert->WidgetTree->FindWidget(Name));
		TestTrue(*FString::Printf(TEXT("%s disabled during generation"),*Name.ToString()),Button&&!Button->GetIsEnabled());
	}
	TestTrue(TEXT("Origin inputs disabled during generation"),Latitude&&!Latitude->GetIsEnabled()&&Longitude&&!Longitude->GetIsEnabled()&&Altitude&&!Altitude->GetIsEnabled());

C->Shutdown(); GI->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowCanonicalDroneIdTest,
	"DroneGCS.UI.RouteWorkflow.AutoStart.CanonicalDroneId",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowCanonicalDroneIdTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	for (const TPair<FString,uint64>& Value : {
		TPair<FString,uint64>(TEXT("Drone001"),1),
		TPair<FString,uint64>(TEXT("Drone010"),10),
		TPair<FString,uint64>(TEXT("Drone999"),999),
		TPair<FString,uint64>(TEXT("Drone1000"),1000),
		TPair<FString,uint64>(TEXT("Drone20000"),20000)})
	{
		FTCHARToUTF8 Encoded(*Value.Key);
		uint64 Parsed=0;
		TestTrue(*FString::Printf(TEXT("Canonical parse %s"),*Value.Key),
			dronegcs::identity::TryParseDroneLogicalId(
				std::string_view(Encoded.Get(),Encoded.Length()),Parsed));
		TestEqual(*FString::Printf(TEXT("Numeric %s"),*Value.Key),Parsed,Value.Value);
		TestEqual(*FString::Printf(TEXT("Round-trip %s"),*Value.Key),
			FString(UTF8_TO_TCHAR(dronegcs::identity::FormatDroneLogicalId(Parsed).c_str())),
			Value.Key);
	}
	const TArray<FString> InvalidValues={FString(),TEXT("Drone1"),TEXT("Drone01"),
		TEXT("Drone000"),TEXT("Drone0"),TEXT("Drone0001"),TEXT("DroneABC"),
		TEXT("drone001"),TEXT("DRONE001"),TEXT(" Drone001"),TEXT("Drone001 "),
		TEXT("Drone-1"),TEXT("Drone001x"),TEXT("Drone18446744073709551616")};
	for (const FString& Invalid : InvalidValues)
	{
		FTCHARToUTF8 Encoded(*Invalid);
		uint64 Parsed=123;
		TestFalse(*FString::Printf(TEXT("Reject %s"),*Invalid),
			dronegcs::identity::TryParseDroneLogicalId(
				std::string_view(Encoded.Get(),Encoded.Length()),Parsed));
		TestEqual(TEXT("Rejected parse clears output"),Parsed,static_cast<uint64>(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowAutoStartExactlyOnceTest,
	"DroneGCS.UI.RouteWorkflow.AutoStart.ExactlyOnce",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowAutoStartExactlyOnceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>();
	C->SetAppliedMaxConcurrentTargetsForTest(6);
	FGCSRouteAutoStartSnapshot Base;
	Base.SelectedDroneIDs={TEXT("Drone001"),TEXT("Drone002"),TEXT("Drone010")};
	Base.FleetSessionID=TEXT("fleet-a"); Base.FleetRevision=7;
	Base.ArtifactSetID=TEXT("artifact-a"); Base.ArtifactSetDigest=FString::ChrN(64,TEXT('a'));
	Base.SourceDatasetDigest=FString::ChrN(64,TEXT('b')); Base.RouteSlotCount=1000;
	for(const TPair<FString,int32>& Mapping : {
		TPair<FString,int32>(TEXT("Drone001"),0),TPair<FString,int32>(TEXT("Drone002"),1),
		TPair<FString,int32>(TEXT("Drone010"),9)})
	{
		FGCSRouteAutoStartTargetSnapshot Target; Target.DroneID=Mapping.Key;
		Target.EndpointIdentity=TEXT("127.0.0.1:")+FString::FromInt(14550+Mapping.Value);
		Target.NumericDroneID=Mapping.Value+1; Target.RouteSlotIndex=Mapping.Value;
		Target.RouteRelativePath=FString::Printf(TEXT("routes/%d.waypoints"),Mapping.Value);
		Target.RouteFileByteSize=100; Target.RouteDigest=FString::ChrN(64,TEXT('c'));
		Base.Targets.Add(Target);
	}
	int32 BuildCalls=0,FreezeCalls=0,SyncCalls=0,StartCalls=0;
	FGCSRouteAutoStartSyncObservation Observation;
	Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::SyncingToConsole);
	FGCSRouteAutoStartTestHooks Hooks;
	Hooks.CaptureSnapshot=[Base](FGCSRouteAutoStartSnapshot& Out,FString&){Out=Base;return true;};
	Hooks.BuildAssignment=[&BuildCalls](FGCSRouteAutoStartSnapshot&,FString&){++BuildCalls;return true;};
	Hooks.FreezeAssignment=[&FreezeCalls](FGCSRouteAutoStartSnapshot& S,FString&){++FreezeCalls;S.AssignmentAuthoritySessionID=TEXT("authority-a");S.AssignmentRevision=8;return true;};
	Hooks.StartSync=[&SyncCalls,&Observation](FGCSRouteAutoStartSnapshot& S,FString&){++SyncCalls;S.SyncTransactionID=TEXT("sync-a");S.AssignmentDigest=FString::ChrN(64,TEXT('d'));Observation.TransactionID=S.SyncTransactionID;Observation.FleetSessionID=S.FleetSessionID;Observation.AssignmentAuthoritySessionID=S.AssignmentAuthoritySessionID;Observation.AssignmentRevision=S.AssignmentRevision;Observation.AssignmentDigest=S.AssignmentDigest;return true;};
	Hooks.ObserveSync=[&Observation](){return Observation;};
	Hooks.ValidateSnapshot=[](const FGCSRouteAutoStartSnapshot&,FString&){return true;};
	Hooks.StartJob=[&StartCalls](int32 Maximum,FString& OutID,FString&){++StartCalls;OutID=TEXT("job-a");return Maximum==6;};
	C->SetAutoStartTestHooks(MoveTemp(Hooks));
	FString Error;
	TestTrue(TEXT("Begin automatic transaction"),C->StartAutoStartForTest(Error));
	TestEqual(TEXT("One Working Assignment"),BuildCalls,1);
	TestEqual(TEXT("One Freeze"),FreezeCalls,1);
	TestEqual(TEXT("One Sync"),SyncCalls,1);
	TestEqual(TEXT("No Job before Accepted"),StartCalls,0);
	TestFalse(TEXT("Repeated click rejected"),C->StartAutoStartForTest(Error));
	Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted);
	C->TickRefreshForTest(); C->TickRefreshForTest(); C->TickRefreshForTest();
	TestEqual(TEXT("Start exactly once"),StartCalls,1);
	TestEqual(TEXT("Completed state"),C->GetAutoStartStateForTest(),EGCSRouteAutoStartState::Completed);
	TestTrue(TEXT("Start dispatched flag"),C->GetAutoStartSnapshotForTest().bJobStartDispatched);
	TestEqual(TEXT("Three selected targets preserved"),C->GetAutoStartSnapshotForTest().Targets.Num(),3);
	TestEqual(TEXT("Applied concurrency frozen into AutoStart"),
		C->GetAutoStartSnapshotForTest().MaxConcurrentTargets,6);
	Observation.TransactionID=TEXT("late-job-a-sync");
	C->TickRefreshForTest();
	TestEqual(TEXT("Late cross-transaction Accepted cannot restart"),StartCalls,1);
	C->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowAutoStartSingleTargetTest,
	"DroneGCS.UI.RouteWorkflow.AutoStart.SingleTarget",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowAutoStartSingleTargetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>();
	FGCSRouteAutoStartSnapshot Base; Base.SelectedDroneIDs={TEXT("Drone001")};
	Base.FleetSessionID=TEXT("fleet-single"); Base.FleetRevision=3;
	Base.ArtifactSetID=TEXT("artifact-single"); Base.ArtifactSetDigest=FString::ChrN(64,TEXT('a'));
	Base.SourceDatasetDigest=FString::ChrN(64,TEXT('b')); Base.RouteSlotCount=1000;
	FGCSRouteAutoStartTargetSnapshot Target; Target.DroneID=TEXT("Drone001");
	Target.EndpointIdentity=TEXT("127.0.0.1:14550"); Target.NumericDroneID=1;
	Target.RouteSlotIndex=0; Target.RouteRelativePath=TEXT("manifest/routes/slot-zero.waypoints");
	Target.RouteFileByteSize=100; Target.RouteDigest=FString::ChrN(64,TEXT('c'));
	Base.Targets.Add(Target);
	int32 BuildCalls=0,FreezeCalls=0,SyncCalls=0,StartCalls=0;
	FGCSRouteAutoStartSyncObservation Observation;
	Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::SyncingToConsole);
	FGCSRouteAutoStartTestHooks Hooks;
	Hooks.CaptureSnapshot=[Base](FGCSRouteAutoStartSnapshot& Out,FString&){Out=Base;return true;};
	Hooks.BuildAssignment=[&BuildCalls](FGCSRouteAutoStartSnapshot&,FString&){++BuildCalls;return true;};
	Hooks.FreezeAssignment=[&FreezeCalls](FGCSRouteAutoStartSnapshot& S,FString&){++FreezeCalls;S.AssignmentAuthoritySessionID=TEXT("authority-single");S.AssignmentRevision=4;return true;};
	Hooks.StartSync=[&SyncCalls,&Observation](FGCSRouteAutoStartSnapshot& S,FString&)
	{
		++SyncCalls; S.SyncTransactionID=TEXT("sync-single"); S.AssignmentDigest=FString::ChrN(64,TEXT('d'));
		Observation.TransactionID=S.SyncTransactionID; Observation.FleetSessionID=S.FleetSessionID;
		Observation.AssignmentAuthoritySessionID=S.AssignmentAuthoritySessionID;
		Observation.AssignmentRevision=S.AssignmentRevision; Observation.AssignmentDigest=S.AssignmentDigest;
		return true;
	};
	Hooks.ObserveSync=[&Observation](){return Observation;};
	Hooks.ValidateSnapshot=[](const FGCSRouteAutoStartSnapshot&,FString&){return true;};
	Hooks.StartJob=[&StartCalls](int32 Maximum,FString& JobID,FString&){++StartCalls;JobID=TEXT("job-single");return Maximum==1;};
	C->SetAutoStartTestHooks(MoveTemp(Hooks)); FString Error;
	TestTrue(TEXT("Single target transaction begins"),C->StartAutoStartForTest(Error));
	Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted);
	C->AdvanceAutoStartForTest(); C->AdvanceAutoStartForTest();
	TestEqual(TEXT("Single target builds once"),BuildCalls,1);
	TestEqual(TEXT("Single target freezes once"),FreezeCalls,1);
	TestEqual(TEXT("Single target syncs once"),SyncCalls,1);
	TestEqual(TEXT("Single target starts once"),StartCalls,1);
	TestEqual(TEXT("Single target count"),C->GetAutoStartSnapshotForTest().Targets.Num(),1);
	TestEqual(TEXT("Drone001 maps to RouteSlot 0"),C->GetAutoStartSnapshotForTest().Targets[0].RouteSlotIndex,0);
	TestEqual(TEXT("Manifest record path is preserved"),
		C->GetAutoStartSnapshotForTest().Targets[0].RouteRelativePath,
		FString(TEXT("manifest/routes/slot-zero.waypoints")));
	C->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowAutoStartFailClosedTest,
	"DroneGCS.UI.RouteWorkflow.AutoStart.FailClosed",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowAutoStartFailClosedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	AddExpectedError(TEXT("[ROUTE_AUTO_START]"),EAutomationExpectedErrorFlags::Contains,4);
	auto RunScenario=[this](const FString& Name,const bool bSyncFailure,const bool bStaleAccepted)
	{
		UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>();
		FGCSRouteAutoStartSnapshot Base; Base.SelectedDroneIDs={TEXT("Drone001")};
		Base.FleetSessionID=TEXT("fleet-a"); Base.FleetRevision=1; Base.ArtifactSetID=TEXT("artifact-a");
		Base.ArtifactSetDigest=FString::ChrN(64,TEXT('a')); Base.SourceDatasetDigest=FString::ChrN(64,TEXT('b')); Base.RouteSlotCount=1000;
		FGCSRouteAutoStartTargetSnapshot Target; Target.DroneID=TEXT("Drone001");Target.EndpointIdentity=TEXT("127.0.0.1:14550");Target.NumericDroneID=1;Target.RouteSlotIndex=0;Target.RouteRelativePath=TEXT("routes/0.waypoints");Target.RouteFileByteSize=100;Target.RouteDigest=FString::ChrN(64,TEXT('c'));Base.Targets.Add(Target);
		int32 StartCalls=0; FGCSRouteAutoStartSyncObservation Observation;
		Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::SyncingToConsole);
		FGCSRouteAutoStartTestHooks Hooks;
		Hooks.CaptureSnapshot=[Base](FGCSRouteAutoStartSnapshot& Out,FString&){Out=Base;return true;};
		Hooks.BuildAssignment=[](FGCSRouteAutoStartSnapshot&,FString&){return true;};
		Hooks.FreezeAssignment=[](FGCSRouteAutoStartSnapshot& S,FString&){S.AssignmentAuthoritySessionID=TEXT("authority-a");S.AssignmentRevision=2;return true;};
		Hooks.StartSync=[&Observation](FGCSRouteAutoStartSnapshot& S,FString&){S.SyncTransactionID=TEXT("sync-a");S.AssignmentDigest=FString::ChrN(64,TEXT('d'));Observation.TransactionID=S.SyncTransactionID;Observation.FleetSessionID=S.FleetSessionID;Observation.AssignmentAuthoritySessionID=S.AssignmentAuthoritySessionID;Observation.AssignmentRevision=S.AssignmentRevision;Observation.AssignmentDigest=S.AssignmentDigest;return true;};
		Hooks.ObserveSync=[&Observation](){return Observation;};
		Hooks.ValidateSnapshot=[bStaleAccepted](const FGCSRouteAutoStartSnapshot&,FString& Error){if(bStaleAccepted){Error=TEXT("Synthetic selection/fleet/endpoint/artifact/record stale");return false;}return true;};
		Hooks.StartJob=[&StartCalls](int32,FString&,FString&){++StartCalls;return true;};
		C->SetAutoStartTestHooks(MoveTemp(Hooks)); FString Error;
		TestTrue(*Name,C->StartAutoStartForTest(Error));
		if(bSyncFailure){Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::SyncFailed);Observation.LastError=TEXT("Synthetic Console reject/timeout");}
		else{Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted);}
		C->AdvanceAutoStartForTest();
		TestEqual(TEXT("Rejected scenario does not Start Job"),StartCalls,0);
		TestTrue(TEXT("Rejected scenario is terminal"),C->GetAutoStartStateForTest()==EGCSRouteAutoStartState::Failed||C->GetAutoStartStateForTest()==EGCSRouteAutoStartState::CancelledAsStale);
		C->Shutdown();
	};
	RunScenario(TEXT("AutoStartConsoleReject"),true,false);
	RunScenario(TEXT("AutoStartStaleAuthorities"),false,true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowAutoStartPreflightRejectMatrixTest,
	"DroneGCS.UI.RouteWorkflow.AutoStart.PreflightRejectMatrix",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowAutoStartPreflightRejectMatrixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Rejections={
		TEXT("No drones selected"),TEXT("Unknown selected drone"),TEXT("Selected drone offline"),
		TEXT("Invalid canonical DroneID"),TEXT("Duplicate numeric DroneID"),
		TEXT("Duplicate RouteSlot"),TEXT("RouteSlot out of range"),
		TEXT("Manifest Route Record missing"),TEXT("Artifact is not READY"),
		TEXT("Upload job already active")};
	for(const FString& Rejection : Rejections)
	{
		UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>();
		int32 BuildCalls=0,FreezeCalls=0,SyncCalls=0,StartCalls=0;
		FGCSRouteAutoStartTestHooks Hooks;
		Hooks.CaptureSnapshot=[Rejection](FGCSRouteAutoStartSnapshot&,FString& Error)
		{
			Error=Rejection;
			return false;
		};
		Hooks.BuildAssignment=[&BuildCalls](FGCSRouteAutoStartSnapshot&,FString&){++BuildCalls;return true;};
		Hooks.FreezeAssignment=[&FreezeCalls](FGCSRouteAutoStartSnapshot&,FString&){++FreezeCalls;return true;};
		Hooks.StartSync=[&SyncCalls](FGCSRouteAutoStartSnapshot&,FString&){++SyncCalls;return true;};
		Hooks.StartJob=[&StartCalls](int32,FString&,FString&){++StartCalls;return true;};
		C->SetAutoStartTestHooks(MoveTemp(Hooks));
		FString Error;
		TestFalse(*Rejection,C->StartAutoStartForTest(Error));
		TestEqual(*FString::Printf(TEXT("%s returns its diagnostic"),*Rejection),Error,Rejection);
		TestEqual(TEXT("Rejected preflight does not build"),BuildCalls,0);
		TestEqual(TEXT("Rejected preflight does not Freeze"),FreezeCalls,0);
		TestEqual(TEXT("Rejected preflight does not Sync"),SyncCalls,0);
		TestEqual(TEXT("Rejected preflight does not Start Job"),StartCalls,0);
		C->Shutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSRouteWorkflowAutoStartStaleMatrixTest,
	"DroneGCS.UI.RouteWorkflow.AutoStart.StaleMatrix",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSRouteWorkflowAutoStartStaleMatrixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> StaleReasons={TEXT("Command Selection changed"),TEXT("Fleet Session changed"),
		TEXT("Fleet Revision changed"),TEXT("endpoint changed"),TEXT("Online state changed"),
		TEXT("ArtifactSetID changed"),TEXT("Manifest digest changed"),
		TEXT("Route record digest/path/size changed")};
	AddExpectedError(TEXT("[ROUTE_AUTO_START]"),EAutomationExpectedErrorFlags::Contains,StaleReasons.Num()*2);
	for(const FString& StaleReason : StaleReasons)
	{
		UGCSRouteWorkflowCoordinator* C=NewObject<UGCSRouteWorkflowCoordinator>();
		FGCSRouteAutoStartSnapshot Base; Base.SelectedDroneIDs={TEXT("Drone001")};
		Base.FleetSessionID=TEXT("fleet-a"); Base.FleetRevision=1;
		Base.ArtifactSetID=TEXT("artifact-a"); Base.ArtifactSetDigest=FString::ChrN(64,TEXT('a'));
		Base.SourceDatasetDigest=FString::ChrN(64,TEXT('b')); Base.RouteSlotCount=1000;
		FGCSRouteAutoStartTargetSnapshot Target; Target.DroneID=TEXT("Drone001");
		Target.EndpointIdentity=TEXT("127.0.0.1:14550"); Target.NumericDroneID=1;
		Target.RouteSlotIndex=0; Target.RouteRelativePath=TEXT("routes/0.waypoints");
		Target.RouteFileByteSize=100; Target.RouteDigest=FString::ChrN(64,TEXT('c'));
		Base.Targets.Add(Target);
		int32 StartCalls=0;
		FGCSRouteAutoStartSyncObservation Observation;
		Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::SyncingToConsole);
		FGCSRouteAutoStartTestHooks Hooks;
		Hooks.CaptureSnapshot=[Base](FGCSRouteAutoStartSnapshot& Out,FString&){Out=Base;return true;};
		Hooks.BuildAssignment=[](FGCSRouteAutoStartSnapshot&,FString&){return true;};
		Hooks.FreezeAssignment=[](FGCSRouteAutoStartSnapshot& S,FString&){S.AssignmentAuthoritySessionID=TEXT("authority-a");S.AssignmentRevision=2;return true;};
		Hooks.StartSync=[&Observation](FGCSRouteAutoStartSnapshot& S,FString&)
		{
			S.SyncTransactionID=TEXT("sync-a"); S.AssignmentDigest=FString::ChrN(64,TEXT('d'));
			Observation.TransactionID=S.SyncTransactionID; Observation.FleetSessionID=S.FleetSessionID;
			Observation.AssignmentAuthoritySessionID=S.AssignmentAuthoritySessionID;
			Observation.AssignmentRevision=S.AssignmentRevision; Observation.AssignmentDigest=S.AssignmentDigest;
			return true;
		};
		Hooks.ObserveSync=[&Observation](){return Observation;};
		Hooks.ValidateSnapshot=[StaleReason](const FGCSRouteAutoStartSnapshot&,FString& Error){Error=StaleReason;return false;};
		Hooks.StartJob=[&StartCalls](int32,FString&,FString&){++StartCalls;return true;};
		C->SetAutoStartTestHooks(MoveTemp(Hooks)); FString Error;
		TestTrue(*FString::Printf(TEXT("Begin %s"),*StaleReason),C->StartAutoStartForTest(Error));
		Observation.State=static_cast<uint8>(EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted);
		C->AdvanceAutoStartForTest();
		TestEqual(*FString::Printf(TEXT("%s cannot Start Job"),*StaleReason),StartCalls,0);
		TestEqual(TEXT("Changed authority cancels transaction as stale"),
			C->GetAutoStartStateForTest(),EGCSRouteAutoStartState::CancelledAsStale);
		C->Shutdown();
	}
	return true;
}

#endif
