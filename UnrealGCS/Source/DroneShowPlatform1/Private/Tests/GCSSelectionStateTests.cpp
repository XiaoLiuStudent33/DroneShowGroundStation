#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "DroneData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSUIBridgeSubsystem.h"

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
	if (!TestNotNull(TEXT("FleetManager subsystem initialized"), Fleet)
		|| !TestNotNull(TEXT("GCSUIBridge subsystem initialized"), Bridge))
	{
		GameInstance->Shutdown();
		return false;
	}

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

	UClass* MainWidgetClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
	UUserWidget* MainWidget = MainWidgetClass
		? CreateWidget<UUserWidget>(GameInstance, MainWidgetClass)
		: nullptr;
	TestNotNull(TEXT("Main widget asset loaded"), MainWidget);
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
	TestNotNull(TEXT("Drone001 row has independent SelectedOverlay"), Row1Border);
	TestNotNull(TEXT("Drone002 row has independent SelectedOverlay"), Row2Border);
	TestNotNull(TEXT("Drone001 command Checkbox remains independent"), Row1Indicator);
	TestNotNull(TEXT("Drone002 command Checkbox remains independent"), Row2Indicator);
	if (MainWidget && Row1Border && Row2Border && Row1Indicator && Row2Indicator)
	{
		const FLinearColor Row1CheckboxColor = Row1Indicator->GetBackgroundColor();
		const FLinearColor Row2CheckboxColor = Row2Indicator->GetBackgroundColor();
		AddExpectedError(
			TEXT("Unable to load GCS navigation page"),
			EAutomationExpectedErrorFlags::Contains,
			3);
		Bridge->BindMainWidget(MainWidget);

		Fleet->SelectDrone(TEXT("Drone001"));
		TestEqual(TEXT("Drone001 overlay is visible"), Row1Border->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
		TestEqual(TEXT("Drone002 overlay is hidden"), Row2Border->GetVisibility(), ESlateVisibility::Hidden);

		Fleet->SelectDrone(TEXT("Drone002"));
		TestEqual(TEXT("Previous Drone001 overlay is hidden"), Row1Border->GetVisibility(), ESlateVisibility::Hidden);
		TestEqual(TEXT("Drone002 overlay is visible"), Row2Border->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
		TestTrue(TEXT("Drone001 checkbox visual is untouched"), Row1Indicator->GetBackgroundColor().Equals(Row1CheckboxColor));
		TestTrue(TEXT("Drone002 checkbox visual is untouched"), Row2Indicator->GetBackgroundColor().Equals(Row2CheckboxColor));
		TestEqual(TEXT("Command selection remains independent"), Bridge->GetSelectedCommandDroneIDs().Num(), 2);
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
		OfflineBatch.FleetRevision = 1;
		FDroneDataDelta OfflineDelta;
		OfflineDelta.DroneID = TEXT("Drone002");
		OfflineDelta.ChangedFields = static_cast<int32>(EGCSDroneDataField::Online);
		OfflineDelta.Data.bOnline = false;
		OfflineBatch.Deltas.Add(OfflineDelta);
		TestTrue(TEXT("Drone002 offline delta applied"), Fleet->ApplyDeltaBatch(OfflineBatch));
		TestEqual(TEXT("Offline drone remains in fleet"), Fleet->GetDroneCount(), 100);
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
	}

	GameInstance->Shutdown();
	return true;
}

#endif
