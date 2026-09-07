#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
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

	Bridge->MapPageWidget = MapClass ? CreateWidget<UUserWidget>(GameInstance, MapClass) : nullptr;
	Bridge->PlaybackWidget = PlaybackClass ? CreateWidget<UUserWidget>(GameInstance, PlaybackClass) : nullptr;
	Bridge->LogsPageWidget = LogsClass ? CreateWidget<UUserWidget>(GameInstance, LogsClass) : nullptr;
	if (!TestNotNull(TEXT("Map page loads"), Bridge->MapPageWidget.Get())
		|| !TestNotNull(TEXT("Playback widget loads"), Bridge->PlaybackWidget.Get())
		|| !TestNotNull(TEXT("Logs page loads"), Bridge->LogsPageWidget.Get()))
	{
		GameInstance->Shutdown();
		return false;
	}

	Bridge->MapPageWidget->SetVisibility(ESlateVisibility::Collapsed);
	Bridge->PlaybackWidget->SetVisibility(ESlateVisibility::Collapsed);
	Bridge->LogsPageWidget->SetVisibility(ESlateVisibility::Collapsed);

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
