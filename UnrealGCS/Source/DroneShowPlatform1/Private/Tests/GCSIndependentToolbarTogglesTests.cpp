#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "DroneData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSUIBridgeSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSIndependentToolbarTogglesTest,
	"DroneGCS.UI.Navigation.IndependentMapPlaybackLogsToggles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSIndependentToolbarTogglesTest::RunTest(const FString&)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("IndependentToolbarTogglesWorld"));
	UGCSUIBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();

	const TSubclassOf<UUserWidget> MapClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_PageMap.WBP_GCS_PageMap_C"));
	const TSubclassOf<UUserWidget> PlaybackClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_Playback.WBP_GCS_Playback_C"));
	const TSubclassOf<UUserWidget> LogsClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_PageLogs.WBP_GCS_PageLogs_C"));
	const TSubclassOf<UUserWidget> FleetPanelClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_FleetPanel_V2.WBP_GCS_FleetPanel_V2_C"));
	const TSubclassOf<UUserWidget> DroneDetailClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_DroneDetail.WBP_GCS_DroneDetail_C"));

	Bridge->MapPageWidget = MapClass ? CreateWidget<UUserWidget>(GameInstance, MapClass) : nullptr;
	Bridge->PlaybackWidget = PlaybackClass ? CreateWidget<UUserWidget>(GameInstance, PlaybackClass) : nullptr;
	Bridge->LogsPageWidget = LogsClass ? CreateWidget<UUserWidget>(GameInstance, LogsClass) : nullptr;
	Bridge->FleetPanelWidget = FleetPanelClass
		? CreateWidget<UUserWidget>(GameInstance, FleetPanelClass)
		: nullptr;
	Bridge->DroneDetailWidget = DroneDetailClass
		? CreateWidget<UUserWidget>(GameInstance, DroneDetailClass)
		: nullptr;
	if (!TestNotNull(TEXT("Map page loads"), Bridge->MapPageWidget.Get())
		|| !TestNotNull(TEXT("Playback widget loads"), Bridge->PlaybackWidget.Get())
		|| !TestNotNull(TEXT("Logs page loads"), Bridge->LogsPageWidget.Get())
		|| !TestNotNull(TEXT("Fleet panel loads"), Bridge->FleetPanelWidget.Get())
		|| !TestNotNull(TEXT("Drone detail loads"), Bridge->DroneDetailWidget.Get()))
	{
		GameInstance->Shutdown();
		return false;
	}

	Bridge->MapPageWidget->SetVisibility(ESlateVisibility::Collapsed);
	Bridge->PlaybackWidget->SetVisibility(ESlateVisibility::Collapsed);
	Bridge->LogsPageWidget->SetVisibility(ESlateVisibility::Collapsed);
	Bridge->bPanelsExpanded = true;
	Bridge->bFleetPanelRequestedVisible = false;
	Bridge->bDroneDetailRequestedVisible = false;
	Bridge->ApplyNavigationVisibility();

	UFleetManagerSubsystem* Fleet =
		GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	FDroneData Drone;
	Drone.DroneID = TEXT("Drone001");
	Drone.bOnline = true;
	TestTrue(TEXT("Primary-panel test drone added"), Fleet && Fleet->AddDrone(Drone));
	TestTrue(TEXT("Operator detail selection accepted"), Fleet && Fleet->SelectDrone(Drone.DroneID));
	Bridge->RefreshFromFleet();
	TestEqual(
		TEXT("Selecting a drone does not open the control panel"),
		Bridge->DroneDetailWidget->GetVisibility(),
		ESlateVisibility::Collapsed);

	Bridge->HandleControlPageClicked();
	TestEqual(
		TEXT("Control button opens the drone detail panel"),
		Bridge->DroneDetailWidget->GetVisibility(),
		ESlateVisibility::Visible);
	Bridge->RefreshFromFleet();
	TestEqual(
		TEXT("Fleet refresh preserves the operator-opened control panel"),
		Bridge->DroneDetailWidget->GetVisibility(),
		ESlateVisibility::Visible);

	Bridge->HandleFleetPageClicked();
	TestEqual(
		TEXT("Fleet button independently opens the fleet panel"),
		Bridge->FleetPanelWidget->GetVisibility(),
		ESlateVisibility::Visible);
	Bridge->HandleMenuClicked();
	TestEqual(
		TEXT("Menu collapses the fleet panel"),
		Bridge->FleetPanelWidget->GetVisibility(),
		ESlateVisibility::Collapsed);
	TestEqual(
		TEXT("Menu collapses the control panel"),
		Bridge->DroneDetailWidget->GetVisibility(),
		ESlateVisibility::Collapsed);
	Bridge->HandleMenuClicked();
	TestEqual(
		TEXT("Menu restores the fleet panel"),
		Bridge->FleetPanelWidget->GetVisibility(),
		ESlateVisibility::Visible);
	TestEqual(
		TEXT("Menu restores the control panel"),
		Bridge->DroneDetailWidget->GetVisibility(),
		ESlateVisibility::Visible);

	Bridge->HandleMapPageClicked();
	TestEqual(TEXT("Map click opens map"), Bridge->MapPageWidget->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
	TestEqual(TEXT("Map click leaves playback closed"), Bridge->PlaybackWidget->GetVisibility(), ESlateVisibility::Collapsed);
	TestEqual(TEXT("Map click leaves logs closed"), Bridge->LogsPageWidget->GetVisibility(), ESlateVisibility::Collapsed);

	Bridge->HandleSettingsPageClicked();
	TestEqual(TEXT("Settings button opens playback"), Bridge->PlaybackWidget->GetVisibility(), ESlateVisibility::Visible);
	TestEqual(TEXT("Playback does not replace map"), Bridge->MapPageWidget->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);

	Bridge->HandleLogsPageClicked();
	TestEqual(TEXT("Logs click opens logs"), Bridge->LogsPageWidget->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
	TestEqual(TEXT("Logs does not replace map"), Bridge->MapPageWidget->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
	TestEqual(TEXT("Logs does not replace playback"), Bridge->PlaybackWidget->GetVisibility(), ESlateVisibility::Visible);

	Bridge->HandleSettingsPageClicked();
	TestEqual(TEXT("Second settings click closes playback"), Bridge->PlaybackWidget->GetVisibility(), ESlateVisibility::Collapsed);
	TestEqual(TEXT("Closing playback leaves map open"), Bridge->MapPageWidget->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
	TestEqual(TEXT("Closing playback leaves logs open"), Bridge->LogsPageWidget->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);

	GameInstance->Shutdown();
	return true;
}

#endif
