#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ListView.h"
#include "DSPGCSProductionCommandProviderSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "DroneData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSUIBridgeSubsystem.h"
#include "HAL/PlatformTime.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSSelectionStateSeparationTest,
	"DroneGCS.UI.Selection.DetailAndCommandStateSeparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSFleetSnapshotDoesNotAutoSelectDetailTest,
	"DroneGCS.UI.Selection.SnapshotDoesNotAutoSelectDetail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSFleetSnapshotDoesNotAutoSelectDetailTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}

	GameInstance->InitializeStandalone(TEXT("GCSSnapshotSelectionTestWorld"));
	UFleetManagerSubsystem* Fleet =
		GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	if (!TestNotNull(TEXT("FleetManager subsystem initialized"), Fleet))
	{
		GameInstance->Shutdown();
		return false;
	}

	FGCSFleetSnapshot InitialSnapshot;
	InitialSnapshot.FleetRevision = 1;
	FDroneData Drone;
	Drone.DroneID = TEXT("Drone000");
	Drone.bOnline = true;
	Drone.bSelected = true;
	InitialSnapshot.Drones.Add(Drone);
	TestTrue(TEXT("Initial snapshot applied"), Fleet->ApplySnapshot(InitialSnapshot));
	TestTrue(
		TEXT("Incoming snapshot selection does not open detail"),
		Fleet->GetCurrentSelectedDroneID().IsEmpty());
	FDroneData SelectedDrone;
	TestFalse(
		TEXT("No detail drone exists before an operator click"),
		Fleet->GetSelectedDrone(SelectedDrone));

	TestTrue(TEXT("Operator can select Drone000"), Fleet->SelectDrone(TEXT("Drone000")));
	FGCSFleetSnapshot RefreshSnapshot = InitialSnapshot;
	RefreshSnapshot.FleetRevision = 2;
	RefreshSnapshot.Drones[0].bSelected = false;
	TestTrue(TEXT("Telemetry refresh snapshot applied"), Fleet->ApplySnapshot(RefreshSnapshot));
	TestEqual(
		TEXT("Telemetry refresh preserves explicit operator detail selection"),
		Fleet->GetCurrentSelectedDroneID(),
		FString(TEXT("Drone000")));

	GameInstance->Shutdown();
	return true;
}

bool FGCSSelectionStateSeparationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}

	GameInstance->InitializeStandalone(TEXT("GCSSelectionStateTestWorld"));
	UFleetManagerSubsystem* Fleet =
		GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	UGCSUIBridgeSubsystem* Bridge =
		GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
	UDSPGCSProductionCommandProviderSubsystem* Commands =
		GameInstance->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>();
	UDSPGCSProductionTelemetryProviderSubsystem* ProductionTelemetry =
		GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>();
	if (!TestNotNull(TEXT("FleetManager subsystem initialized"), Fleet)
		|| !TestNotNull(TEXT("GCSUIBridge subsystem initialized"), Bridge)
		|| !TestNotNull(TEXT("Production command provider initialized"), Commands)
		|| !TestNotNull(TEXT("Production telemetry provider initialized"), ProductionTelemetry))
	{
		GameInstance->Shutdown();
		return false;
	}

	const TArray<FString> ProviderTargets{ TEXT("Drone001") };
	FGCSProductionCommandParameters DefaultCommandParameters;
	TestTrue(
		TEXT("ServerDisconnected rejects Production command"),
		Commands->SubmitCommand(
			EGCSProductionCommandType::Arm,
			ProviderTargets,
			DefaultCommandParameters).IsEmpty());

	ProductionTelemetry->MarkValidTelemetryReceivedAt(FPlatformTime::Seconds());
	TestTrue(TEXT("Production command provider enabled"), Commands->IsEnabled());
	TestTrue(TEXT("Production telemetry link connected"), ProductionTelemetry->IsTelemetryLinkConnected());

	FGCSProductionCommandParameters HoverGateParameters;
	HoverGateParameters.HoverAltitudeMeters = 5.0f;
	TestTrue(
		TEXT("Direct HOVER remains blocked by the MANUAL safety gate"),
		Commands->SubmitCommand(
			EGCSProductionCommandType::HoverTest,
			ProviderTargets,
			HoverGateParameters).IsEmpty());
	Bridge->SetCommandDroneSelected(TEXT("Drone001"), true);
	Bridge->HandleHoverClicked();
	TestEqual(
		TEXT("Operator HOVER enters autonomous workflow mode before submission"),
		Commands->GetControlMode(),
		EGCSGroundStationControlMode::Autonomous);
	TestTrue(
		TEXT("Operator HOVER reports that the workflow was submitted"),
		Bridge->HoverTestStatus.Contains(TEXT("悬停起飞任务已提交")));
	Commands->SetControlMode(EGCSGroundStationControlMode::Manual);

	const auto SubmitAndRequireID = [this, Commands, &ProviderTargets](
		const TCHAR* Label,
		const EGCSProductionCommandType Type,
		const FGCSProductionCommandParameters& CommandParameters)
	{
		TestFalse(
			Label,
			Commands->SubmitCommand(Type, ProviderTargets, CommandParameters).IsEmpty());
	};

	SubmitAndRequireID(TEXT("ARM uses Production Provider"), EGCSProductionCommandType::Arm, DefaultCommandParameters);
	SubmitAndRequireID(TEXT("DISARM uses Production Provider"), EGCSProductionCommandType::Disarm, DefaultCommandParameters);

	FGCSProductionCommandParameters GuidedParameters;
	GuidedParameters.CustomMode = 4;
	GuidedParameters.ModeName = TEXT("GUIDED");
	SubmitAndRequireID(TEXT("GUIDED uses Production SET_MODE"), EGCSProductionCommandType::SetMode, GuidedParameters);

	FGCSProductionCommandParameters StabilizeParameters;
	StabilizeParameters.CustomMode = 0;
	StabilizeParameters.ModeName = TEXT("STABILIZE");
	SubmitAndRequireID(TEXT("STABILIZE uses Production SET_MODE"), EGCSProductionCommandType::SetMode, StabilizeParameters);

	FGCSProductionCommandParameters AutoParameters;
	AutoParameters.CustomMode = 3;
	AutoParameters.ModeName = TEXT("AUTO");
	SubmitAndRequireID(TEXT("AUTO uses Production SET_MODE"), EGCSProductionCommandType::SetMode, AutoParameters);

	FGCSProductionCommandParameters TakeoffParameters;
	TakeoffParameters.TakeoffAltitudeMeters = 1.0f;
	SubmitAndRequireID(TEXT("TAKEOFF uses Production Provider"), EGCSProductionCommandType::Takeoff, TakeoffParameters);
	SubmitAndRequireID(TEXT("LAND uses Production Provider"), EGCSProductionCommandType::Land, DefaultCommandParameters);
	SubmitAndRequireID(TEXT("RTL uses Production Provider"), EGCSProductionCommandType::RTL, DefaultCommandParameters);

	Commands->SetControlMode(EGCSGroundStationControlMode::Autonomous);
	FGCSProductionCommandParameters HoverParameters;
	HoverParameters.HoverAltitudeMeters = 5.0f;
	SubmitAndRequireID(TEXT("HOVER uses Production Provider"), EGCSProductionCommandType::HoverTest, HoverParameters);
	SubmitAndRequireID(TEXT("STOP uses Production LAND"), EGCSProductionCommandType::Land, DefaultCommandParameters);

	for (int32 Index = 1; Index <= 4; ++Index)
	{
		FDroneData Drone;
		Drone.DroneID = FString::Printf(TEXT("Drone%03d"), Index);
		Drone.Mode = FString::Printf(TEXT("MODE_%d"), Index);
		Drone.GPSLat = 31.0 + Index;
		Drone.GPSLon = 118.0 + Index;
		Drone.Battery = 90.0f + Index;
		Drone.Altitude = static_cast<float>(Index);
		Drone.RSSI = -40 - Index;
		Drone.bOnline = true;
		TestTrue(TEXT("Drone added"), Fleet->AddDrone(Drone));
	}
	for (int32 Index = 5; Index <= 100; ++Index)
	{
		FDroneData Drone;
		Drone.DroneID = FString::Printf(TEXT("Drone%03d"), Index);
		Drone.Mode = TEXT("STABILIZE");
		Drone.Battery = 90.0f;
		Drone.bOnline = true;
		TestTrue(TEXT("Fleet status test drone added"), Fleet->AddDrone(Drone));
	}

	FDroneData Detail;
	TestTrue(TEXT("Drone001 detail row selected"), Fleet->SelectDrone(TEXT("Drone001")));
	TestEqual(TEXT("Current detail ID is Drone001"), Fleet->GetCurrentSelectedDroneID(), FString(TEXT("Drone001")));
	TestTrue(TEXT("Drone001 detail snapshot found"), Fleet->GetSelectedDrone(Detail));
	TestEqual(TEXT("Detail displays Drone001"), Detail.DroneID, FString(TEXT("Drone001")));
	UE_LOG(LogTemp, Display, TEXT("[SELECTION_TEST] RowClick Drone001 CurrentSelectedDroneID=Drone001 Detail=Drone001"));

	TestTrue(TEXT("Drone002 detail row selected"), Fleet->SelectDrone(TEXT("Drone002")));
	TestEqual(TEXT("Current detail ID is Drone002"), Fleet->GetCurrentSelectedDroneID(), FString(TEXT("Drone002")));
	TestTrue(TEXT("Drone002 detail snapshot found"), Fleet->GetSelectedDrone(Detail));
	TestEqual(TEXT("Detail displays Drone002"), Detail.DroneID, FString(TEXT("Drone002")));
	UE_LOG(LogTemp, Display, TEXT("[SELECTION_TEST] RowClick Drone002 CurrentSelectedDroneID=Drone002 Detail=Drone002"));

	Bridge->SetCommandDroneSelected(TEXT("Drone001"), true);
	Bridge->SetCommandDroneSelected(TEXT("Drone003"), true);
	const TArray<FString> CommandTargets = Bridge->GetSelectedCommandDroneIDs();
	TestEqual(TEXT("Two command targets checked"), CommandTargets.Num(), 2);
	TestTrue(TEXT("Drone001 is a command target"), CommandTargets.Contains(TEXT("Drone001")));
	TestTrue(TEXT("Drone003 is a command target"), CommandTargets.Contains(TEXT("Drone003")));
	TestEqual(
		TEXT("Command checks do not change CurrentSelectedDroneID"),
		Fleet->GetCurrentSelectedDroneID(),
		FString(TEXT("Drone002")));
	TestTrue(TEXT("Drone002 detail snapshot still found"), Fleet->GetSelectedDrone(Detail));
	TestEqual(TEXT("Detail still displays Drone002"), Detail.DroneID, FString(TEXT("Drone002")));
	UE_LOG(LogTemp, Display, TEXT("[SELECTION_TEST] Check Drone001,Drone003 Targets=[Drone001,Drone003] CurrentSelectedDroneID=Drone002 Detail=Drone002"));

	TestTrue(TEXT("Drone004 detail row selected"), Fleet->SelectDrone(TEXT("Drone004")));
	TestEqual(TEXT("Current detail ID is Drone004"), Fleet->GetCurrentSelectedDroneID(), FString(TEXT("Drone004")));
	TestTrue(TEXT("Drone004 detail snapshot found"), Fleet->GetSelectedDrone(Detail));
	TestEqual(TEXT("Detail displays Drone004"), Detail.DroneID, FString(TEXT("Drone004")));
	TestEqual(TEXT("Command targets remain selected"), Bridge->GetSelectedCommandDroneIDs().Num(), 2);
	UE_LOG(LogTemp, Display, TEXT("[SELECTION_TEST] RowClick Drone004 CurrentSelectedDroneID=Drone004 Detail=Drone004 Targets=[Drone001,Drone003]"));

	Bridge->ReplaceCommandDroneSelection(
		{ TEXT("Drone002"), TEXT("Drone006"), TEXT("Drone011") });
	const TArray<FString> ReplacedTargets = Bridge->GetSelectedCommandDroneIDs();
	TestEqual(TEXT("Marquee replacement stores global command targets"), ReplacedTargets.Num(), 3);
	TestTrue(TEXT("Replacement includes Drone002"), ReplacedTargets.Contains(TEXT("Drone002")));
	TestTrue(TEXT("Replacement includes cross-page Drone006"), ReplacedTargets.Contains(TEXT("Drone006")));
	TestTrue(TEXT("Replacement includes cross-page Drone011"), ReplacedTargets.Contains(TEXT("Drone011")));
	TestEqual(
		TEXT("Command replacement does not change detail selection"),
		Fleet->GetCurrentSelectedDroneID(),
		FString(TEXT("Drone004")));
	Bridge->ReplaceCommandDroneSelection({});
	TestEqual(TEXT("Empty marquee clears command selection"), Bridge->GetSelectedCommandDroneIDs().Num(), 0);
	Bridge->ReplaceCommandDroneSelection({ TEXT("Drone001"), TEXT("Drone006") });

	UClass* MainWidgetClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
	UUserWidget* MainWidget = MainWidgetClass
		? CreateWidget<UUserWidget>(GameInstance, MainWidgetClass)
		: nullptr;
	TestNotNull(TEXT("Main widget asset loaded"), MainWidget);
	UBorder* SelectionRectangle = MainWidget && MainWidget->WidgetTree
		? Cast<UBorder>(MainWidget->WidgetTree->FindWidget(TEXT("SelectionRectangle")))
		: nullptr;
	TestNotNull(TEXT("Main widget has authored marquee rectangle"), SelectionRectangle);
	if (SelectionRectangle)
	{
		TestEqual(
			TEXT("Marquee rectangle starts collapsed"),
			SelectionRectangle->GetVisibility(),
			ESlateVisibility::Collapsed);
	}
	UUserWidget* FleetPanel = MainWidget && MainWidget->WidgetTree
		? Cast<UUserWidget>(MainWidget->WidgetTree->FindWidget(TEXT("CHILD_FleetPanel")))
		: nullptr;
	TestNotNull(TEXT("Fleet panel child found"), FleetPanel);

	UListView* AuthoredFleetList = FleetPanel && FleetPanel->WidgetTree
		? Cast<UListView>(FleetPanel->WidgetTree->FindWidget(TEXT("LV_FleetDrones")))
		: nullptr;
	if (!TestNotNull(TEXT("Fleet panel uses authored LV_FleetDrones"), AuthoredFleetList))
	{
		GameInstance->Shutdown();
		return false;
	}

	AddExpectedError(
		TEXT("Unable to load GCS navigation page"),
		EAutomationExpectedErrorFlags::Contains,
		2);
	Bridge->BindMainWidget(MainWidget);
	TestEqual(
		TEXT("Fleet ListView delegates detail selection to its row button"),
		AuthoredFleetList->GetSelectionMode(),
		ESelectionMode::None);
	TestEqual(
		TEXT("Binding the ListView preserves explicit detail selection"),
		Fleet->GetCurrentSelectedDroneID(),
		FString(TEXT("Drone004")));
	TestEqual(
		TEXT("Binding the ListView preserves command selection"),
		Bridge->GetSelectedCommandDroneIDs().Num(),
		2);

	UButton* MissionPauseButton = Bridge->FindRightPanelButton(TEXT("BTN_Pause"));
	UButton* MissionForwardButton = Bridge->FindRightPanelButton(TEXT("BTN_MoveForward"));
	UButton* MissionBackwardButton = Bridge->FindRightPanelButton(TEXT("BTN_MoveBackward"));
	if (!TestNotNull(TEXT("Mission pause button found"), MissionPauseButton)
		|| !TestNotNull(TEXT("Mission forward button found"), MissionForwardButton)
		|| !TestNotNull(TEXT("Mission backward button found"), MissionBackwardButton))
	{
		GameInstance->Shutdown();
		return false;
	}
	TestTrue(TEXT("Mission pause starts enabled"), MissionPauseButton->GetIsEnabled());
	TestFalse(TEXT("Mission forward starts disabled"), MissionForwardButton->GetIsEnabled());
	TestFalse(TEXT("Mission backward starts disabled"), MissionBackwardButton->GetIsEnabled());

	MissionPauseButton->OnClicked.Broadcast();
	TestFalse(
		TEXT("Pause click creates a pending production command"),
		Bridge->PendingMissionPlaybackCommandID.IsEmpty());
	TestEqual(
		TEXT("Pause click records the selected target count"),
		Bridge->PendingMissionPlaybackTargetCount,
		2);
	TestEqual(
		TEXT("Pause click records mission-pause command type"),
		Bridge->PendingMissionPlaybackCommandType,
		EGCSProductionCommandType::MissionPause);
	TestFalse(
		TEXT("Mission forward stays disabled while pause results are pending"),
		MissionForwardButton->GetIsEnabled());

	const FString PauseCommandID = Bridge->PendingMissionPlaybackCommandID;
	for (const FString& DroneID : Bridge->GetSelectedCommandDroneIDs())
	{
		FGCSProductionCommandResult PauseResult;
		PauseResult.CommandID = PauseCommandID;
		PauseResult.DroneID = DroneID;
		PauseResult.CommandType = EGCSProductionCommandType::MissionPause;
		PauseResult.AckState = TEXT("ACCEPTED");
		PauseResult.State = EGCSCommandResultState::Accepted;
		PauseResult.bFinal = true;
		Bridge->HandleProductionCommandResult(PauseResult);
	}
	TestTrue(
		TEXT("Mission forward unlocks after every selected drone acknowledges pause"),
		MissionForwardButton->GetIsEnabled());
	TestTrue(
		TEXT("Mission backward unlocks after every selected drone acknowledges pause"),
		MissionBackwardButton->GetIsEnabled());

	MissionForwardButton->OnClicked.Broadcast();
	TestEqual(
		TEXT("Forward button submits the mission-forward command"),
		Bridge->PendingMissionPlaybackCommandType,
		EGCSProductionCommandType::MissionForward);
	TestFalse(
		TEXT("Direction buttons relock after forward is submitted"),
		MissionBackwardButton->GetIsEnabled());

	GameInstance->Shutdown();
	return true;
}

namespace
{
	bool RunFleetSelectAllScaleCase(
		FAutomationTestBase& Test,
		const int32 DroneCount,
		const TCHAR* WorldName)
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
		if (!Test.TestNotNull(TEXT("Scale GameInstance created"), GameInstance))
		{
			return false;
		}
		GameInstance->InitializeStandalone(WorldName);
		UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
		UGCSUIBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
		if (!Test.TestNotNull(TEXT("Scale FleetManager initialized"), Fleet)
			|| !Test.TestNotNull(TEXT("Scale UI bridge initialized"), Bridge))
		{
			GameInstance->Shutdown();
			return false;
		}

		FGCSFleetSnapshot Snapshot;
		Snapshot.FleetRevision = 1;
		Snapshot.Drones.Reserve(DroneCount);
		for (int32 Index = 1; Index <= DroneCount; ++Index)
		{
			FDroneData Drone;
			Drone.DroneID = FString::Printf(TEXT("Drone%05d"), Index);
			Drone.bOnline = true;
			Snapshot.Drones.Add(MoveTemp(Drone));
		}
		Test.TestTrue(TEXT("Scale snapshot applied"), Fleet->ApplySnapshot(Snapshot));
		int32 BroadcastCount = 0;
		const FDelegateHandle Handle = Bridge->OnCommandSelectionChanged.AddLambda(
			[&BroadcastCount]() { ++BroadcastCount; });
		const double StartedAt = FPlatformTime::Seconds();
		Bridge->SelectAllEligibleFleetForTest();
		const double DurationMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;
		Test.TestEqual(TEXT("One bulk replacement selects the whole fleet"),
			Bridge->GetSelectedCommandDroneIDs().Num(), DroneCount);
		Test.TestEqual(TEXT("Bulk replacement broadcasts once"), BroadcastCount, 1);
		Test.TestEqual(TEXT("Eligible scan covers the whole fleet"),
			Bridge->GetEligibleOnlineCommandDroneIDsForTest().Num(), DroneCount);
		UE_LOG(LogTemp, Display,
			TEXT("[FLEET_SELECT_ALL_SCALE] Drones=%d DurationMs=%.3f Broadcasts=%d WidgetsCreated=0"),
			DroneCount, DurationMs, BroadcastCount);
		Bridge->OnCommandSelectionChanged.Remove(Handle);
		GameInstance->Shutdown();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSFleetSelectAllScale1000Test,
	"DroneGCS.UI.FleetSelectAll.Scale1000",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSFleetSelectAllScale1000Test::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunFleetSelectAllScaleCase(*this, 1000, TEXT("GCSFleetSelectAllScale1000"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSFleetSelectAllScale20000Test,
	"DroneGCS.UI.FleetSelectAll.Scale20000",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSFleetSelectAllScale20000Test::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunFleetSelectAllScaleCase(*this, 20000, TEXT("GCSFleetSelectAllScale20000"));
}

#endif

#include "GCSRouteWorkflowCoordinatorTests.inl"
