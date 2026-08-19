#if WITH_DEV_AUTOMATION_TESTS

#include "GCSRouteWorkflowCoordinator.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/TextBlock.h"
#include "DSPGCSUploadJobProtocol.h"
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
	for(const FName Name:{FName(TEXT("BTN_StartUpload")),FName(TEXT("BTN_PauseUpload")),FName(TEXT("BTN_ResumeUpload")),FName(TEXT("BTN_CancelUpload")),FName(TEXT("BTN_RetryFailed"))})
	{
		UButton* Button=Upload&&Upload->WidgetTree?Cast<UButton>(Upload->WidgetTree->FindWidget(Name)):nullptr;
		TestNotNull(*FString::Printf(TEXT("%s resolved"),*Name.ToString()),Button);
		if(Button) TestFalse(*FString::Printf(TEXT("%s disabled"),*Name.ToString()),Button->GetIsEnabled());
	}
	UEditableTextBox* Concurrency=Upload&&Upload->WidgetTree?Cast<UEditableTextBox>(Upload->WidgetTree->FindWidget(TEXT("EDT_MaxConcurrency"))):nullptr;
	TestNotNull(TEXT("Concurrency resolved"),Concurrency);
	if(Concurrency) { TestTrue(TEXT("Concurrency read only"),Concurrency->GetIsReadOnly()); TestFalse(TEXT("Concurrency disabled"),Concurrency->GetIsEnabled()); }
	TestEqual(TEXT("Created once"),C->GetWorkflowCreationCountForTest(),1);
	TestTrue(TEXT("Ticker active"),C->IsRefreshTickerActiveForTest());
	TestTrue(TEXT("Repeated Show"),C->ShowRouteWorkflow(Error));
	TestTrue(TEXT("Repeated Show reuses"),C->GetWorkflowWidgetForTest()==First);
	TestEqual(TEXT("Still created once"),C->GetWorkflowCreationCountForTest(),1);
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
	TestEqual(TEXT("throughput"),UGCSRouteWorkflowCoordinator::FormatThroughput(125*1024),FString(TEXT("125 KB/s")));
	TestEqual(TEXT("elapsed"),UGCSRouteWorkflowCoordinator::FormatElapsedMilliseconds(92000),FString(TEXT("00:01:32")));
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

#endif
