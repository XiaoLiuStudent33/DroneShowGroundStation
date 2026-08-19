#if WITH_DEV_AUTOMATION_TESTS

#include "GCSRouteWorkflowCoordinator.h"
#include "Components/EditableTextBox.h"

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
	return true;
}

#endif
