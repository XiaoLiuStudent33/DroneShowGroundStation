#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "DSPGCSUploadJobSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GCSUIBridgeSubsystem.h"

namespace GCSUploadDashboardToggleTest
{
	struct FWidgetFixture
	{
		UUserWidget* Main = nullptr;
		UUserWidget* RightToolbar = nullptr;
		UButton* DeviceButton = nullptr;
		UButton* OtherButton = nullptr;
		UCanvasPanel* MainCanvas = nullptr;
		UCanvasPanelSlot* ToolbarSlot = nullptr;
	};

	FWidgetFixture MakeFixture(UGameInstance* GameInstance)
	{
		FWidgetFixture Fixture;
		const TSubclassOf<UUserWidget> MainClass = LoadClass<UUserWidget>(
			nullptr,
			TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
		if (!MainClass)
		{
			return Fixture;
		}

		Fixture.Main = CreateWidget<UUserWidget>(GameInstance, MainClass);
		if (!Fixture.Main || !Fixture.Main->WidgetTree)
		{
			return Fixture;
		}
		Fixture.MainCanvas = Cast<UCanvasPanel>(Fixture.Main->WidgetTree->RootWidget);
		Fixture.RightToolbar = Cast<UUserWidget>(
			Fixture.Main->WidgetTree->FindWidget(TEXT("CHILD_RightToolbar")));
		Fixture.ToolbarSlot = Fixture.RightToolbar
			? Cast<UCanvasPanelSlot>(Fixture.RightToolbar->Slot)
			: nullptr;
		Fixture.DeviceButton = Fixture.RightToolbar && Fixture.RightToolbar->WidgetTree
			? Cast<UButton>(Fixture.RightToolbar->WidgetTree->FindWidget(TEXT("BTN_Device")))
			: nullptr;
		Fixture.OtherButton = Fixture.RightToolbar && Fixture.RightToolbar->WidgetTree
			? Cast<UButton>(Fixture.RightToolbar->WidgetTree->FindWidget(TEXT("BTN_View")))
			: nullptr;
		return Fixture;
	}

	int32 CountDashboardInstances(
		const UCanvasPanel* MainCanvas,
		const UClass* DashboardClass)
	{
		int32 Count = 0;
		if (!MainCanvas || !DashboardClass)
		{
			return Count;
		}
		for (int32 ChildIndex = 0; ChildIndex < MainCanvas->GetChildrenCount(); ++ChildIndex)
		{
			const UWidget* Child = MainCanvas->GetChildAt(ChildIndex);
			Count += Child && Child->IsA(DashboardClass) ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSUploadDashboardToggleTest,
	"DroneGCS.UI.UploadDashboardToggle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSUploadDashboardToggleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace GCSUploadDashboardToggleTest;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}
	GameInstance->InitializeStandalone(TEXT("GCSUploadDashboardToggleTestWorld"));
	UGCSUIBridgeSubsystem* Bridge =
		GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
	UDSPGCSUploadJobSubsystem* UploadJobs =
		GameInstance->GetSubsystem<UDSPGCSUploadJobSubsystem>();
	FWidgetFixture Fixture = MakeFixture(GameInstance);
	if (!TestNotNull(TEXT("UI Bridge initialized"), Bridge)
		|| !TestNotNull(TEXT("Upload Job subsystem initialized"), UploadJobs)
		|| !TestNotNull(TEXT("Runtime main widget created"), Fixture.Main)
		|| !TestNotNull(TEXT("Main root is CanvasPanel"), Fixture.MainCanvas)
		|| !TestNotNull(TEXT("CHILD_RightToolbar resolved"), Fixture.RightToolbar)
		|| !TestNotNull(TEXT("RightToolbar Canvas slot resolved"), Fixture.ToolbarSlot)
		|| !TestNotNull(TEXT("BTN_Device resolved"), Fixture.DeviceButton))
	{
		GameInstance->Shutdown();
		return false;
	}

	Bridge->BindUploadDashboardForTest(Fixture.Main, Fixture.RightToolbar);
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[UploadDashboardTest] ToolbarAnchor=(%.3f,%.3f) Position=(%.3f,%.3f) Size=(%.3f,%.3f) Alignment=(%.3f,%.3f)"),
		Fixture.ToolbarSlot->GetAnchors().Minimum.X,
		Fixture.ToolbarSlot->GetAnchors().Minimum.Y,
		Fixture.ToolbarSlot->GetPosition().X,
		Fixture.ToolbarSlot->GetPosition().Y,
		Fixture.ToolbarSlot->GetSize().X,
		Fixture.ToolbarSlot->GetSize().Y,
		Fixture.ToolbarSlot->GetAlignment().X,
		Fixture.ToolbarSlot->GetAlignment().Y);
	TestTrue(
		TEXT("Authored RightToolbar uses a fixed top-right anchor"),
		Fixture.ToolbarSlot->GetAnchors().Minimum.Equals(FVector2D(1.0f, 0.0f))
			&& Fixture.ToolbarSlot->GetAnchors().Maximum.Equals(FVector2D(1.0f, 0.0f)));
	TestTrue(
		TEXT("BTN_Device dynamic delegate is bound"),
		Bridge->IsUploadDashboardToggleBoundForTest());
	TestNull(TEXT("Dashboard is not eagerly created"), Bridge->GetUploadDashboardForTest());
	TestFalse(TEXT("Initial dashboard state is closed"), Bridge->IsUploadDashboardVisibleForTest());

	UploadJobs->BindUploadJobForTest("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	const FDSPGCSUploadJobCommandStatus CommandBefore = UploadJobs->GetCommandStatus();
	const FDSPGCSUploadJobMirrorStatus MirrorBefore = UploadJobs->GetMirrorStatus();
	TestTrue(
		TEXT("Dashboard remains viewable with an active Upload Job binding"),
		CommandBefore.bHasActiveJobBinding);
	Fixture.DeviceButton->OnClicked.Broadcast();
	UUserWidget* FirstDashboard = Bridge->GetUploadDashboardForTest();
	if (!TestNotNull(TEXT("First click loads and creates the authored Dashboard"), FirstDashboard))
	{
		GameInstance->Shutdown();
		return false;
	}
	TestTrue(
		TEXT("Production class is WBP_GCS_UploadDashboard"),
		FirstDashboard
			&& FirstDashboard->GetClass()->GetName().Contains(TEXT("WBP_GCS_UploadDashboard")));
	TestTrue(TEXT("First click records visible state"), Bridge->IsUploadDashboardVisibleForTest());
	TestEqual(
		TEXT("First click makes Dashboard Visible"),
		FirstDashboard ? FirstDashboard->GetVisibility() : ESlateVisibility::Collapsed,
		ESlateVisibility::Visible);
	TestEqual(
		TEXT("Main Canvas contains exactly one Dashboard"),
		CountDashboardInstances(
			Fixture.MainCanvas,
			FirstDashboard ? FirstDashboard->GetClass() : nullptr),
		1);
	FirstDashboard->ForceLayoutPrepass();
	const FVector2D DashboardDesiredSize = FirstDashboard->GetDesiredSize();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[UploadDashboardTest] DesiredSize=(%.3f,%.3f)"),
		DashboardDesiredSize.X,
		DashboardDesiredSize.Y);
	if (const UCanvasPanel* DashboardRoot =
		Cast<UCanvasPanel>(FirstDashboard->WidgetTree->RootWidget))
	{
		for (int32 ChildIndex = 0; ChildIndex < DashboardRoot->GetChildrenCount(); ++ChildIndex)
		{
			const UWidget* Child = DashboardRoot->GetChildAt(ChildIndex);
			const UCanvasPanelSlot* ChildSlot = Child
				? Cast<UCanvasPanelSlot>(Child->Slot)
				: nullptr;
			if (ChildSlot)
			{
				UE_LOG(
					LogTemp,
					Display,
					TEXT("[UploadDashboardTest] RootChild=%s Position=(%.3f,%.3f) Size=(%.3f,%.3f) Alignment=(%.3f,%.3f) AutoSize=%s Anchors=(%.3f,%.3f)-(%.3f,%.3f)"),
					*GetNameSafe(Child),
					ChildSlot->GetPosition().X,
					ChildSlot->GetPosition().Y,
					ChildSlot->GetSize().X,
					ChildSlot->GetSize().Y,
					ChildSlot->GetAlignment().X,
					ChildSlot->GetAlignment().Y,
					ChildSlot->GetAutoSize() ? TEXT("true") : TEXT("false"),
					ChildSlot->GetAnchors().Minimum.X,
					ChildSlot->GetAnchors().Minimum.Y,
					ChildSlot->GetAnchors().Maximum.X,
					ChildSlot->GetAnchors().Maximum.Y);
			}
		}
	}
	TestTrue(
		TEXT("Canvas-root Dashboard may report zero desired size"),
		DashboardDesiredSize.IsNearlyZero());

	UCanvasPanelSlot* DashboardSlot = FirstDashboard
		? Cast<UCanvasPanelSlot>(FirstDashboard->Slot)
		: nullptr;
	TestNotNull(TEXT("Dashboard is hosted by the main Canvas"), DashboardSlot);
	if (DashboardSlot)
	{
		TestTrue(
			TEXT("Dashboard reuses the toolbar anchor"),
			DashboardSlot->GetAnchors().Minimum.Equals(
				Fixture.ToolbarSlot->GetAnchors().Minimum));
		TestTrue(
			TEXT("Dashboard uses right-edge alignment"),
			DashboardSlot->GetAlignment().Equals(FVector2D(1.0f, 0.0f)));
		const float ToolbarLeft = Fixture.ToolbarSlot->GetPosition().X
			- Fixture.ToolbarSlot->GetSize().X * Fixture.ToolbarSlot->GetAlignment().X;
		TestTrue(
			TEXT("Dashboard right edge remains left of RightToolbar"),
			DashboardSlot->GetPosition().X < ToolbarLeft);
		const float GapAt1280 =
			(1280.0f * DashboardSlot->GetAnchors().Minimum.X + ToolbarLeft)
			- (1280.0f * DashboardSlot->GetAnchors().Minimum.X
				+ DashboardSlot->GetPosition().X);
		const float GapAt2560 =
			(2560.0f * DashboardSlot->GetAnchors().Minimum.X + ToolbarLeft)
			- (2560.0f * DashboardSlot->GetAnchors().Minimum.X
				+ DashboardSlot->GetPosition().X);
		TestTrue(TEXT("Resize preserves toolbar gap"), FMath::IsNearlyEqual(GapAt1280, GapAt2560));
		TestFalse(TEXT("Dashboard uses an explicit authored Canvas size"), DashboardSlot->GetAutoSize());
		TestTrue(
			TEXT("Dashboard outer slot has a non-zero authored size"),
			DashboardSlot->GetSize().X > 1.0f && DashboardSlot->GetSize().Y > 1.0f);
		TestTrue(
			TEXT("Dashboard outer slot matches authored root-child bounds"),
			DashboardSlot->GetSize().Equals(FVector2D(480.0f, 760.0f)));
	}

	dspgcs::MavFtpRuntimeConfigSnapshotDTO RuntimeConfig;
	RuntimeConfig.authoritySessionId = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	RuntimeConfig.configRevision = 1;
	RuntimeConfig.bootDefaultGlobalWriteCap = 16;
	RuntimeConfig.runtimeAcceptedGlobalWriteCap = 64;
	RuntimeConfig.writeWindow = 16;
	RuntimeConfig.writePayload = 239;
	RuntimeConfig.segmentItems = 4000;
	RuntimeConfig.requestTimeoutMs = 750;
	RuntimeConfig.terminateTimeoutMs = 2500;
	RuntimeConfig.retryCount = 3;
	std::string RuntimeConfigError;
	TestTrue(TEXT("Runtime Config digest is created"),
		dspgcs::ComputeMavFtpRuntimeConfigDigest(
			RuntimeConfig, RuntimeConfig.configDigest, RuntimeConfigError));
	UploadJobs->SetMavFtpRuntimeConfigForTest(RuntimeConfig);
	Bridge->RefreshUploadDashboardForTest();

	auto FindEdit = [FirstDashboard](const FName Name) -> UWidget*
	{
		return FirstDashboard && FirstDashboard->WidgetTree
			? FirstDashboard->WidgetTree->FindWidget(Name)
			: nullptr;
	};
	auto IsReadOnly = [](const UWidget* Widget)
	{
		if (const UEditableText* Input = Cast<UEditableText>(Widget))
			return Input->GetIsReadOnly();
		if (const UEditableTextBox* Input = Cast<UEditableTextBox>(Widget))
			return Input->GetIsReadOnly();
		return false;
	};
	auto InputText = [](const UWidget* Widget)
	{
		if (const UEditableText* Input = Cast<UEditableText>(Widget))
			return Input->GetText().ToString();
		if (const UEditableTextBox* Input = Cast<UEditableTextBox>(Widget))
			return Input->GetText().ToString();
		return FString();
	};
	UWidget* WriteWindow = FindEdit(TEXT("EDT_MavFtpWriteWindow"));
	UWidget* GlobalCap = FindEdit(TEXT("EDT_MavFtpGlobalWriteCap"));
	UWidget* SegmentItems = FindEdit(TEXT("EDT_MavFtpSegmentItems"));
	TestNotNull(TEXT("Write Window control resolved"), WriteWindow);
	TestNotNull(TEXT("Global Cap control resolved"), GlobalCap);
	TestNotNull(TEXT("Segment Items control resolved"), SegmentItems);
	if (WriteWindow && GlobalCap && SegmentItems)
	{
		TestTrue(TEXT("Write Window is read-only"), IsReadOnly(WriteWindow));
		TestFalse(TEXT("Global Cap is the editable Draft"), IsReadOnly(GlobalCap));
		TestTrue(TEXT("Segment Items is read-only"), IsReadOnly(SegmentItems));
		TestEqual(TEXT("Write Window comes from Console Snapshot"),
			InputText(WriteWindow), FString(TEXT("16")));
		TestEqual(TEXT("Global Cap comes from Console Snapshot"),
			InputText(GlobalCap), FString(TEXT("64")));
		TestEqual(TEXT("Segment Items comes from Console Snapshot"),
			InputText(SegmentItems), FString(TEXT("4000")));
	}

	UWidget* DashboardInput = FirstDashboard && FirstDashboard->WidgetTree
		? FirstDashboard->WidgetTree->FindWidget(TEXT("EDT_MavFtpGlobalWriteCap"))
		: nullptr;
	TestNotNull(TEXT("Authored Dashboard input resolved"), DashboardInput);
	if (UEditableText* EditableText = Cast<UEditableText>(DashboardInput))
	{
		const FText DraftText = FText::FromString(TEXT("13"));
		EditableText->SetText(DraftText);
		EditableText->OnTextChanged.Broadcast(DraftText);
	}
	else if (UEditableTextBox* EditableTextBox = Cast<UEditableTextBox>(DashboardInput))
	{
		const FText DraftText = FText::FromString(TEXT("13"));
		EditableTextBox->SetText(DraftText);
		EditableTextBox->OnTextChanged.Broadcast(DraftText);
	}
	Fixture.DeviceButton->OnClicked.Broadcast();
	TestFalse(TEXT("Second click closes Dashboard"), Bridge->IsUploadDashboardVisibleForTest());
	TestEqual(
		TEXT("Closed Dashboard is Collapsed"),
		FirstDashboard ? FirstDashboard->GetVisibility() : ESlateVisibility::Visible,
		ESlateVisibility::Collapsed);
	Fixture.DeviceButton->OnClicked.Broadcast();
	TestTrue(TEXT("Third click reopens Dashboard"), Bridge->IsUploadDashboardVisibleForTest());
	TestTrue(TEXT("Third click reuses the same Widget"), Bridge->GetUploadDashboardForTest() == FirstDashboard);
	if (const UEditableText* EditableText = Cast<UEditableText>(DashboardInput))
	{
		TestEqual(
			TEXT("Collapsed Dashboard retains draft text"),
			EditableText->GetText().ToString(),
			FString(TEXT("13")));
	}
	else if (const UEditableTextBox* EditableTextBox = Cast<UEditableTextBox>(DashboardInput))
	{
		TestEqual(
			TEXT("Collapsed Dashboard retains draft text"),
			EditableTextBox->GetText().ToString(),
			FString(TEXT("13")));
	}
	else
	{
		AddError(TEXT("EDT_MavFtpGlobalWriteCap has an unsupported widget type."));
	}

	RuntimeConfig.configRevision = 2;
	RuntimeConfig.runtimeAcceptedGlobalWriteCap = 96;
	TestTrue(TEXT("New Runtime Config digest is created"),
		dspgcs::ComputeMavFtpRuntimeConfigDigest(
			RuntimeConfig, RuntimeConfig.configDigest, RuntimeConfigError));
	UploadJobs->SetMavFtpRuntimeConfigForTest(RuntimeConfig);
	Bridge->RefreshUploadDashboardForTest();
	TestEqual(TEXT("Dirty Draft survives a newer Snapshot"),
		Bridge->GetUploadDashboardDraftGlobalCapForTest(), 13);
	TestEqual(TEXT("Dirty Draft becomes STALE on a newer revision"),
		Bridge->GetUploadDashboardTuningStatusForTest(), FString(TEXT("STALE")));
	Bridge->RestoreUploadDashboardForTest();
	TestEqual(TEXT("Restore uses Boot Default without Apply"),
		Bridge->GetUploadDashboardDraftGlobalCapForTest(), 16);
	TestTrue(TEXT("Restore leaves a modified local Draft"),
		Bridge->IsUploadDashboardDraftDirtyForTest());

	for (int32 ClickIndex = 0; ClickIndex < 20; ++ClickIndex)
	{
		Fixture.DeviceButton->OnClicked.Broadcast();
	}
	TestTrue(TEXT("Even rapid clicks preserve state parity"), Bridge->IsUploadDashboardVisibleForTest());
	TestEqual(
		TEXT("Rapid clicks preserve one Dashboard instance"),
		CountDashboardInstances(Fixture.MainCanvas, FirstDashboard->GetClass()),
		1);

	const int32 ToggleCountBeforeRebind = Bridge->GetUploadDashboardToggleCountForTest();
	Bridge->BindUploadDashboardForTest(Fixture.Main, Fixture.RightToolbar);
	Bridge->BindUploadDashboardForTest(Fixture.Main, Fixture.RightToolbar);
	Fixture.DeviceButton->OnClicked.Broadcast();
	TestEqual(
		TEXT("Repeated binding dispatches one toggle"),
		Bridge->GetUploadDashboardToggleCountForTest(),
		ToggleCountBeforeRebind + 1);
	TestFalse(TEXT("One post-rebind click changes state once"), Bridge->IsUploadDashboardVisibleForTest());
	TestTrue(TEXT("Rebinding preserves singleton object"), Bridge->GetUploadDashboardForTest() == FirstDashboard);
	FWidgetFixture ReplacementFixture = MakeFixture(GameInstance);
	if (TestNotNull(TEXT("Replacement main widget created"), ReplacementFixture.Main)
		&& TestNotNull(TEXT("Replacement BTN_Device resolved"), ReplacementFixture.DeviceButton))
	{
		Bridge->BindUploadDashboardForTest(
			ReplacementFixture.Main,
			ReplacementFixture.RightToolbar);
		TestTrue(
			TEXT("Main-widget replacement migrates the same Dashboard"),
			Bridge->GetUploadDashboardForTest() == FirstDashboard);
		TestTrue(
			TEXT("Dashboard is reparented to the replacement main Canvas"),
			FirstDashboard->GetParent() == ReplacementFixture.MainCanvas);
		Fixture = ReplacementFixture;
	}

	const FDSPGCSUploadJobCommandStatus CommandAfter = UploadJobs->GetCommandStatus();
	const FDSPGCSUploadJobMirrorStatus MirrorAfter = UploadJobs->GetMirrorStatus();
	TestEqual(
		TEXT("Toggle does not create or bind an Upload Job"),
		CommandAfter.ActiveUploadJobID,
		CommandBefore.ActiveUploadJobID);
	TestEqual(
		TEXT("Toggle does not send an Upload Job command"),
		CommandAfter.bCommandInFlight,
		CommandBefore.bCommandInFlight);
	TestEqual(
		TEXT("Toggle does not create an aggregate snapshot"),
		MirrorAfter.bHasAuthoritativeSnapshot,
		MirrorBefore.bHasAuthoritativeSnapshot);
	TestNotNull(TEXT("Other toolbar button remains present"), Fixture.OtherButton);

	Bridge->ResetUploadDashboardForTest();
	TestNull(TEXT("Teardown clears Dashboard reference"), Bridge->GetUploadDashboardForTest());
	TestNull(TEXT("Teardown clears toggle button reference"), Bridge->GetUploadDashboardToggleButtonForTest());
	TestFalse(TEXT("Teardown resets visibility state"), Bridge->IsUploadDashboardVisibleForTest());
	TestFalse(TEXT("Teardown removes BTN_Device delegate"), Bridge->IsUploadDashboardToggleBoundForTest());
	TestNull(TEXT("Teardown removes Dashboard from the main Canvas"), FirstDashboard->GetParent());

	AddExpectedError(
		TEXT("BTN_Device was not found"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Bridge->BindUploadDashboardForTest(Fixture.Main, nullptr);
	TestNull(
		TEXT("Missing BTN_Device fails closed"),
		Bridge->GetUploadDashboardToggleButtonForTest());

	Bridge->SetUploadDashboardFactoryForTest([]() { return nullptr; });
	Bridge->BindUploadDashboardForTest(Fixture.Main, Fixture.RightToolbar);
	AddExpectedError(
		TEXT("Unable to create the Upload Dashboard widget"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Fixture.DeviceButton->OnClicked.Broadcast();
	TestNull(TEXT("Creation failure leaves no Dashboard"), Bridge->GetUploadDashboardForTest());
	TestFalse(TEXT("Creation failure remains closed"), Bridge->IsUploadDashboardVisibleForTest());
	Bridge->ResetUploadDashboardForTest();
	GameInstance->Shutdown();

	UGameInstance* SecondGameInstance = NewObject<UGameInstance>(GEngine);
	SecondGameInstance->InitializeStandalone(TEXT("GCSUploadDashboardSecondRuntimeTestWorld"));
	UGCSUIBridgeSubsystem* SecondBridge =
		SecondGameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
	FWidgetFixture SecondFixture = MakeFixture(SecondGameInstance);
	if (TestNotNull(TEXT("Second runtime UI Bridge"), SecondBridge)
		&& TestNotNull(TEXT("Second runtime BTN_Device"), SecondFixture.DeviceButton))
	{
		SecondBridge->BindUploadDashboardForTest(
			SecondFixture.Main,
			SecondFixture.RightToolbar);
		TestNull(
			TEXT("Second runtime does not inherit old Dashboard"),
			SecondBridge->GetUploadDashboardForTest());
		TestFalse(
			TEXT("Second runtime starts closed"),
			SecondBridge->IsUploadDashboardVisibleForTest());
	}
	SecondGameInstance->Shutdown();

	return true;
}

#endif
