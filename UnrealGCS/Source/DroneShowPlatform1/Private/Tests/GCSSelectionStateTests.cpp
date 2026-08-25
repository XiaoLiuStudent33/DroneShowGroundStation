#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "DSPGCSProductionCommandProviderSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "DSPGCSUploadJobSubsystem.h"
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

	UBorder* Row1Border = FleetPanel && FleetPanel->WidgetTree
		? Cast<UBorder>(FleetPanel->WidgetTree->FindWidget(TEXT("SelectedOverlay_Row1")))
		: nullptr;
	UBorder* Row2Border = FleetPanel && FleetPanel->WidgetTree
		? Cast<UBorder>(FleetPanel->WidgetTree->FindWidget(TEXT("SelectedOverlay_Row2")))
		: nullptr;
	UButton* Row1Indicator = FleetPanel && FleetPanel->WidgetTree
		? Cast<UButton>(FleetPanel->WidgetTree->FindWidget(TEXT("BTN_FleetSelectRow1")))
		: nullptr;
	UButton* Row2Indicator = FleetPanel && FleetPanel->WidgetTree
		? Cast<UButton>(FleetPanel->WidgetTree->FindWidget(TEXT("BTN_FleetSelectRow2")))
		: nullptr;
	UButton* FleetSelectAllButton = FleetPanel && FleetPanel->WidgetTree
		? Cast<UButton>(FleetPanel->WidgetTree->FindWidget(TEXT("BTN_FleetSelectALL")))
		: nullptr;
	UTextBlock* FleetSelectAllText = FleetPanel && FleetPanel->WidgetTree
		? Cast<UTextBlock>(FleetPanel->WidgetTree->FindWidget(TEXT("BTN_FleetSelectALL_Text")))
		: nullptr;
	TestNotNull(TEXT("Drone001 row has independent SelectedOverlay"), Row1Border);
	TestNotNull(TEXT("Drone002 row has independent SelectedOverlay"), Row2Border);
	TestNotNull(TEXT("Drone001 command Checkbox remains independent"), Row1Indicator);
	TestNotNull(TEXT("Drone002 command Checkbox remains independent"), Row2Indicator);
	TestNotNull(TEXT("Fleet select-all button exists"), FleetSelectAllButton);
	TestNotNull(TEXT("Fleet select-all text exists"), FleetSelectAllText);
	if (MainWidget && Row1Border && Row2Border && Row1Indicator && Row2Indicator)
	{
		if (UDSPGCSProductionTelemetryProviderSubsystem* Telemetry =
			GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>())
		{
			Telemetry->MarkValidTelemetryReceivedAt(FPlatformTime::Seconds());
		}
		const FLinearColor Row1CheckboxColor = Row1Indicator->GetBackgroundColor();
		const FLinearColor Row2CheckboxColor = Row2Indicator->GetBackgroundColor();
		AddExpectedError(
			TEXT("Unable to load GCS navigation page"),
			EAutomationExpectedErrorFlags::Contains,
			3);
		Bridge->BindMainWidget(MainWidget);
		if (FleetSelectAllButton && FleetSelectAllText)
		{
			int32 SelectionChangedCount = 0;
			const FDelegateHandle SelectionChangedHandle =
				Bridge->OnCommandSelectionChanged.AddLambda(
					[&SelectionChangedCount]() { ++SelectionChangedCount; });
			TestTrue(TEXT("Select-all enabled with online fleet"), FleetSelectAllButton->GetIsEnabled());
			TestEqual(TEXT("Partial selection displays X"), FleetSelectAllText->GetText().ToString(), FString(TEXT("X")));
			FleetSelectAllButton->OnClicked.Broadcast();
			TestEqual(TEXT("Select-all selects all 100 online drones"), Bridge->GetSelectedCommandDroneIDs().Num(), 100);
			TestEqual(TEXT("Bulk selection broadcasts once"), SelectionChangedCount, 1);
			TestEqual(TEXT("Exact all-online selection displays check"), FleetSelectAllText->GetText().ToString(), FString(TEXT("✓")));
			TestEqual(TEXT("Bulk selection preserves detail selection"), Fleet->GetCurrentSelectedDroneID(), FString(TEXT("Drone004")));

			// Rebinding this control group must remain idempotent: one click clears
			// rather than toggling twice.
			Bridge->RebindFleetPanelButtonsForTest();
			FleetSelectAllButton->OnClicked.Broadcast();
			TestEqual(TEXT("Second toggle clears all command targets"), Bridge->GetSelectedCommandDroneIDs().Num(), 0);
			TestEqual(TEXT("Rebound click broadcasts exactly once"), SelectionChangedCount, 2);
			TestEqual(TEXT("Cleared selection displays X"), FleetSelectAllText->GetText().ToString(), FString(TEXT("X")));
			FleetSelectAllButton->OnClicked.Broadcast();
			TestEqual(TEXT("Cross-page authority reselects all 100"), Bridge->GetSelectedCommandDroneIDs().Num(), 100);
			TestEqual(TEXT("Reselect broadcasts once"), SelectionChangedCount, 3);
			Bridge->OnCommandSelectionChanged.Remove(SelectionChangedHandle);
			Bridge->ReplaceCommandDroneSelection({ TEXT("Drone001"), TEXT("Drone006") });
		}

		Fleet->SelectDrone(TEXT("Drone001"));
		TestEqual(TEXT("Drone001 overlay is visible"), Row1Border->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
		TestEqual(TEXT("Drone002 overlay is hidden"), Row2Border->GetVisibility(), ESlateVisibility::Hidden);

		Fleet->SelectDrone(TEXT("Drone002"));
		TestEqual(TEXT("Previous Drone001 overlay is hidden"), Row1Border->GetVisibility(), ESlateVisibility::Hidden);
		TestEqual(TEXT("Drone002 overlay is visible"), Row2Border->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
		TestTrue(TEXT("Drone001 checkbox visual is untouched"), Row1Indicator->GetBackgroundColor().Equals(Row1CheckboxColor));
		TestTrue(TEXT("Drone002 checkbox visual is untouched"), Row2Indicator->GetBackgroundColor().Equals(Row2CheckboxColor));
		TestEqual(TEXT("Command selection remains independent"), Bridge->GetSelectedCommandDroneIDs().Num(), 2);
		UTextBlock* Row1CommandLabel = Cast<UTextBlock>(Row1Indicator->GetContent());
		UTextBlock* Row2CommandLabel = Cast<UTextBlock>(Row2Indicator->GetContent());
		UTextBlock* SelectedCountText = Cast<UTextBlock>(
			FleetPanel->WidgetTree->FindWidget(TEXT("TXT_FleetSelectedCount")));
		TestNotNull(TEXT("Row1 command indicator keeps direct TextBlock content"), Row1CommandLabel);
		TestNotNull(TEXT("Row2 command indicator keeps direct TextBlock content"), Row2CommandLabel);
		TestNotNull(TEXT("Global selected count text exists"), SelectedCountText);
		if (Row1CommandLabel && Row2CommandLabel && SelectedCountText)
		{
			TestEqual(TEXT("Selected Drone001 row shows check"), Row1CommandLabel->GetText().ToString(), FString(TEXT("✓")));
			TestEqual(TEXT("Unselected Drone002 row shows X"), Row2CommandLabel->GetText().ToString(), FString(TEXT("X")));
			TestEqual(TEXT("Selected count is global"), SelectedCountText->GetText().ToString(), FString(TEXT("SELECTED 2")));
		}
		UE_LOG(LogTemp, Display, TEXT("[SELECTION_VISUAL_TEST] Drone001 overlay hidden; Drone002 overlay shown; checkbox targets unchanged"));
	}

	if (MainWidget && FleetPanel && FleetPanel->WidgetTree)
	{
		const FLinearColor OnlineColor(0.15f, 1.0f, 0.45f, 1.0f);
		const FLinearColor OfflineColor(1.0f, 0.18f, 0.20f, 1.0f);
		TArray<UTextBlock*> StatusRows;
		for (int32 RowIndex = 1; RowIndex <= 5; ++RowIndex)
		{
			UTextBlock* Status = Cast<UTextBlock>(FleetPanel->WidgetTree->FindWidget(
				*FString::Printf(TEXT("TXT_StatusRow%d"), RowIndex)));
			StatusRows.Add(Status);
			TestNotNull(
				*FString::Printf(TEXT("Status row %d exists"), RowIndex),
				Status);
			if (Status)
			{
				TestEqual(
					*FString::Printf(TEXT("Status row %d shows Online"), RowIndex),
					Status->GetText().ToString(),
					FString(TEXT("Online")));
				TestTrue(
					*FString::Printf(TEXT("Status row %d uses online green"), RowIndex),
					Status->GetColorAndOpacity().GetSpecifiedColor().Equals(OnlineColor));
			}
		}
		TestEqual(TEXT("Fleet retains 100 online drones"), Fleet->GetDroneCount(), 100);

		FGCSTelemetryBatch OfflineBatch;
		if (FleetSelectAllButton)
		{
			FleetSelectAllButton->OnClicked.Broadcast();
			TestEqual(TEXT("Pre-offline exact selection includes all 100"), Bridge->GetSelectedCommandDroneIDs().Num(), 100);
		}
		OfflineBatch.FleetRevision = 1;
		FDroneDataDelta OfflineDelta;
		OfflineDelta.DroneID = TEXT("Drone002");
		OfflineDelta.ChangedFields = static_cast<int32>(EGCSDroneDataField::Online);
		OfflineDelta.Data.bOnline = false;
		OfflineBatch.Deltas.Add(OfflineDelta);
		TestTrue(TEXT("Drone002 offline delta applied"), Fleet->ApplyDeltaBatch(OfflineBatch));
		TestEqual(TEXT("Offline drone remains in fleet"), Fleet->GetDroneCount(), 100);
		if (FleetSelectAllButton && FleetSelectAllText)
		{
			TestEqual(TEXT("Offline selected drone invalidates exact-all state"), FleetSelectAllText->GetText().ToString(), FString(TEXT("X")));
			TestEqual(TEXT("Fleet change does not silently alter frozen selection"), Bridge->GetSelectedCommandDroneIDs().Num(), 100);
			FleetSelectAllButton->OnClicked.Broadcast();
			const TArray<FString> OnlineOnlyTargets = Bridge->GetSelectedCommandDroneIDs();
			TestEqual(TEXT("Select-all replaces with 99 online drones"), OnlineOnlyTargets.Num(), 99);
			TestFalse(TEXT("Offline Drone002 excluded"), OnlineOnlyTargets.Contains(TEXT("Drone002")));
			TestEqual(TEXT("Online-only exact set displays check"), FleetSelectAllText->GetText().ToString(), FString(TEXT("✓")));
		}
		if (StatusRows.IsValidIndex(1) && StatusRows[1])
		{
			TestEqual(
				TEXT("Drone002 row shows Offline"),
				StatusRows[1]->GetText().ToString(),
				FString(TEXT("Offline")));
			TestTrue(
				TEXT("Drone002 row uses offline red"),
				StatusRows[1]->GetColorAndOpacity().GetSpecifiedColor().Equals(OfflineColor));
		}

		FGCSTelemetryBatch RecoveryBatch;
		RecoveryBatch.FleetRevision = 2;
		FDroneDataDelta RecoveryDelta;
		RecoveryDelta.DroneID = TEXT("Drone002");
		RecoveryDelta.ChangedFields = static_cast<int32>(EGCSDroneDataField::Online);
		RecoveryDelta.Data.bOnline = true;
		RecoveryBatch.Deltas.Add(RecoveryDelta);
		TestTrue(TEXT("Drone002 recovery delta applied"), Fleet->ApplyDeltaBatch(RecoveryBatch));
		if (FleetSelectAllButton && FleetSelectAllText)
		{
			TestEqual(TEXT("Newly online drone does not auto-select"), Bridge->GetSelectedCommandDroneIDs().Num(), 99);
			TestEqual(TEXT("New online drone changes exact-all display to X"), FleetSelectAllText->GetText().ToString(), FString(TEXT("X")));
			FleetSelectAllButton->OnClicked.Broadcast();
			TestEqual(TEXT("Explicit click includes newly online drone"), Bridge->GetSelectedCommandDroneIDs().Num(), 100);
		}
		if (StatusRows.IsValidIndex(1) && StatusRows[1])
		{
			TestEqual(
				TEXT("Recovered Drone002 row shows Online"),
				StatusRows[1]->GetText().ToString(),
				FString(TEXT("Online")));
			TestTrue(
				TEXT("Recovered Drone002 row uses online green"),
					StatusRows[1]->GetColorAndOpacity().GetSpecifiedColor().Equals(OnlineColor));
		}
		Bridge->ReplaceCommandDroneSelection({ TEXT("Drone001"), TEXT("Drone006") });

		UButton* NextPageButton = Cast<UButton>(
			FleetPanel->WidgetTree->FindWidget(TEXT("BTN_FleetNextPage")));
		TestNotNull(TEXT("Fleet next-page button exists"), NextPageButton);
		if (NextPageButton)
		{
			const FString SelectedBeforePaging = Fleet->GetCurrentSelectedDroneID();
			NextPageButton->OnClicked.Broadcast();
			UTextBlock* Row1Text = Cast<UTextBlock>(
				FleetPanel->WidgetTree->FindWidget(TEXT("Row1")));
			TestTrue(
				TEXT("Second page row one maps to Drone006"),
				Row1Text && Row1Text->GetText().ToString().Contains(TEXT("Drone006")));
			if (StatusRows.IsValidIndex(0) && StatusRows[0])
			{
				TestEqual(
					TEXT("Second page status follows Drone006"),
					StatusRows[0]->GetText().ToString(),
					FString(TEXT("Online")));
			}
			TestEqual(
				TEXT("Paging does not change CurrentSelectedDroneID"),
				Fleet->GetCurrentSelectedDroneID(),
				SelectedBeforePaging);
			TestEqual(
				TEXT("Paging does not change command selection"),
				Bridge->GetSelectedCommandDroneIDs().Num(),
				2);
			UButton* SecondPageRow1Indicator = Cast<UButton>(
				FleetPanel->WidgetTree->FindWidget(TEXT("BTN_FleetSelectRow1")));
			UTextBlock* SecondPageRow1Label = SecondPageRow1Indicator
				? Cast<UTextBlock>(SecondPageRow1Indicator->GetContent())
				: nullptr;
			TestTrue(
				TEXT("Cross-page Drone006 retains command check"),
				SecondPageRow1Label
				&& SecondPageRow1Label->GetText().ToString() == TEXT("✓"));
		}

		FGCSFleetSnapshot SparseSnapshot;
		SparseSnapshot.FleetRevision = 3;
		for (int32 Index = 1; Index <= 2; ++Index)
		{
			FDroneData Drone;
			Drone.DroneID = FString::Printf(TEXT("Drone%03d"), Index);
			Drone.bOnline = true;
			SparseSnapshot.Drones.Add(Drone);
		}
		TestTrue(TEXT("Sparse fleet snapshot applied"), Fleet->ApplySnapshot(SparseSnapshot));
		if (FleetSelectAllButton && FleetSelectAllText)
		{
			TestEqual(TEXT("Stale command IDs invalidate all-selected"), FleetSelectAllText->GetText().ToString(), FString(TEXT("X")));
			FleetSelectAllButton->OnClicked.Broadcast();
			const TArray<FString> SparseTargets = Bridge->GetSelectedCommandDroneIDs();
			TestEqual(TEXT("Sparse fleet click replaces stale set"), SparseTargets.Num(), 2);
			TestTrue(TEXT("Sparse selection contains Drone001"), SparseTargets.Contains(TEXT("Drone001")));
			TestTrue(TEXT("Sparse selection contains Drone002"), SparseTargets.Contains(TEXT("Drone002")));
		}
		for (int32 RowIndex = 2; RowIndex < StatusRows.Num(); ++RowIndex)
		{
			if (StatusRows[RowIndex])
			{
				TestTrue(
					*FString::Printf(TEXT("Blank status row %d has no text"), RowIndex + 1),
					StatusRows[RowIndex]->GetText().IsEmpty());
				TestEqual(
					*FString::Printf(TEXT("Blank status row %d is hidden"), RowIndex + 1),
					StatusRows[RowIndex]->GetVisibility(),
					ESlateVisibility::Hidden);
			}
		}
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[FLEET_STATUS_TEST] total=100 firstPage=Online offlineRetained=Drone002 recovery=Online secondPage=Drone006 selectionUnchanged blankRowsCleared=3"));

		FGCSFleetSnapshot OfflineOnlySnapshot;
		OfflineOnlySnapshot.FleetRevision = 4;
		FDroneData OfflineOnlyDrone;
		OfflineOnlyDrone.DroneID = TEXT("Drone001");
		OfflineOnlyDrone.bOnline = false;
		OfflineOnlySnapshot.Drones.Add(OfflineOnlyDrone);
		TestTrue(TEXT("Offline-only fleet snapshot applied"), Fleet->ApplySnapshot(OfflineOnlySnapshot));
		if (FleetSelectAllButton && FleetSelectAllText)
		{
			const TArray<FString> BeforeDisabledClick = Bridge->GetSelectedCommandDroneIDs();
			TestFalse(TEXT("Select-all disabled with zero online drones"), FleetSelectAllButton->GetIsEnabled());
			TestEqual(TEXT("Zero online displays X"), FleetSelectAllText->GetText().ToString(), FString(TEXT("X")));
			FleetSelectAllButton->OnClicked.Broadcast();
			TestEqual(TEXT("Zero-online click leaves selection unchanged"), Bridge->GetSelectedCommandDroneIDs(), BeforeDisabledClick);
		}

		FGCSFleetSnapshot ActiveGateSnapshot;
		ActiveGateSnapshot.FleetRevision = 5;
		FDroneData ActiveDrone;
		ActiveDrone.DroneID = TEXT("Drone001");
		ActiveDrone.bOnline = true;
		ActiveGateSnapshot.Drones.Add(ActiveDrone);
		TestTrue(TEXT("Active-gate fleet snapshot applied"), Fleet->ApplySnapshot(ActiveGateSnapshot));
		if (UDSPGCSUploadJobSubsystem* Upload =
			GameInstance->GetSubsystem<UDSPGCSUploadJobSubsystem>())
		{
			Upload->BindUploadJobForTest("active-fleet-selection-job");
			Bridge->RefreshFleetSelectAllForTest();
			if (FleetSelectAllButton)
			{
				const TArray<FString> BeforeActiveClick = Bridge->GetSelectedCommandDroneIDs();
				TestFalse(TEXT("Active upload binding disables select-all"), FleetSelectAllButton->GetIsEnabled());
				FleetSelectAllButton->OnClicked.Broadcast();
				TestEqual(TEXT("Active upload binding blocks selection mutation"), Bridge->GetSelectedCommandDroneIDs(), BeforeActiveClick);
			}
		}
	}

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
		Bridge->ToggleFleetSelectAllForTest();
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
