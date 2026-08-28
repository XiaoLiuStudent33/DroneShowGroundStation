#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Blueprint/UserWidget.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "CesiumGeoreference.h"
#include "DroneFleetVisualizationActor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GCSUIBridgeSubsystem.h"
#include "RoutePreviewSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSScheduledShowPreviewTest,
	"DroneGCS.UI.TimeControl.ScheduledPreview10Hz",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSScheduledShowPreviewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GI = NewObject<UGameInstance>(GEngine);
	GI->InitializeStandalone(TEXT("ScheduledShowPreviewTest"));
	ON_SCOPE_EXIT { GI->Shutdown(); };
	URoutePreviewSubsystem* Preview = GI->GetSubsystem<URoutePreviewSubsystem>();
	UGCSUIBridgeSubsystem* Bridge = GI->GetSubsystem<UGCSUIBridgeSubsystem>();
	UWorld* World = GI->GetWorld();
	if (!TestNotNull(TEXT("Preview"), Preview) || !TestNotNull(TEXT("UI bridge"), Bridge)
		|| !TestNotNull(TEXT("World"), World)) return false;
	World->SpawnActor<ACesiumGeoreference>();
	ADroneFleetVisualizationActor* Visualization = World->SpawnActor<ADroneFleetVisualizationActor>();
	if (!TestNotNull(TEXT("Visualization"), Visualization)) return false;

	UClass* RightPanelClass = LoadClass<UUserWidget>(nullptr,
		TEXT("/Game/UI/WBP_GCS_RightPanel.WBP_GCS_RightPanel_C"));
	UClass* PlaybackClass = LoadClass<UUserWidget>(nullptr,
		TEXT("/Game/UI/WBP_GCS_Playback.WBP_GCS_Playback_C"));
	if (!TestNotNull(TEXT("Authored right panel"), RightPanelClass)
		|| !TestNotNull(TEXT("Authored playback"), PlaybackClass)) return false;
	Bridge->RightPanelWidget = CreateWidget<UUserWidget>(GI, RightPanelClass);
	Bridge->PlaybackWidget = CreateWidget<UUserWidget>(GI, PlaybackClass);
	Bridge->BindTimeControlControls();
	Bridge->BindRoutePlaybackControls();
	UTextBlock* Label = Bridge->FindRightPanelText(TEXT("TXT_TimeCountdownLabel"));
	UTextBlock* Elapsed = Bridge->FindRightPanelText(TEXT("TXT_TimeCountdown"));
	if (!TestNotNull(TEXT("Duration label"), Label) || !TestNotNull(TEXT("Duration value"), Elapsed)
		|| !TestTrue(TEXT("Timeline is bound"), Bridge->RouteTimelineSlider.IsValid())) return false;
	TestEqual(TEXT("Label renamed"), Label->GetText().ToString(), FString(TEXT("表演时长")));
	TestEqual(TEXT("Initial elapsed time"), Elapsed->GetText().ToString(), FString(TEXT("00:00:00")));

	// Tiny in-memory route only. No TimeStart/ARM/AUTO handler is invoked and
	// no command is submitted to Console or to physical aircraft by this test.
	FRoutePreviewData Fixture;
	Fixture.FrameCount = 21;
	Fixture.DroneCount = 2;
	FString Error;
	FRouteArtifactDigest256::ParseLowerHex(FString::ChrN(64, TCHAR('a')),
		Fixture.SourceIdentity.SourceDatasetDigest, Error);
	Fixture.SourceIdentity.SourceByteSize = 1;
	Fixture.SourceIdentity.SourceDisplayFileName = TEXT("scheduled-preview-test.npz");
	for (int32 Frame = 0; Frame < Fixture.FrameCount; ++Frame)
	{
		for (int32 Drone = 0; Drone < Fixture.DroneCount; ++Drone)
		{
			Fixture.PositionsENU.Add(FVector3f(Frame, Drone * 3, Frame));
			Fixture.Colors.Add(FFloat16Color(FLinearColor::White));
		}
	}
	Preview->PreparedRouteData = Fixture;
	if (!TestTrue(TEXT("Workflow import"), Preview->CommitPreparedRouteForWorkflow(
		31.9, 118.6, 30.0, false, Error))) return false;
	TestEqual(TEXT("Explicit show profile is 10 Hz"), Preview->GetSampleRateHz(), 10.0);
	TestEqual(TEXT("21 frames span 2 seconds"), Preview->GetDurationSeconds(), 2.0);
	TestTrue(TEXT("Earlier manual inspection"), Preview->SeekToFrame(15, Error));
	Bridge->TimeControlPreviewDatasetGeneration = Preview->GetRouteDatasetGeneration();
	Bridge->ScheduledTimeLocal = FDateTime::Now() - FTimespan::FromSeconds(1.0);
	Bridge->bTimeControlCountdownActive = true;
	Bridge->TickTimeControlUI(0.2f);
	TestTrue(TEXT("Scheduled tick starts playback without pressing Play"),
		Preview->GetPlaybackState() == ERoutePreviewPlaybackState::Playing);
	TestEqual(TEXT("Starts at Frame 0, not inspection cursor"), Preview->GetCurrentFrame(), 0);
	TestEqual(TEXT("Both planned drones rendered"), Visualization->GetVisualizationDroneCount(), 2);
	const FVector InitialPosition = Visualization->GetDronePositions()[0];
	Preview->TickPlayback(1.0f);
	Bridge->TickTimeControlUI(0.2f);
	Bridge->RefreshRoutePlaybackUI();
	TestEqual(TEXT("One second is ten frames"), Preview->GetCurrentFrame(), 10);
	TestEqual(TEXT("Count up"), Elapsed->GetText().ToString(), FString(TEXT("00:00:01")));
	TestEqual(TEXT("Bottom progress advances to half"), Bridge->RouteTimelineSlider->GetValue(), 0.5f);
	TestFalse(TEXT("Rendered sphere moves along route"), Visualization->GetDronePositions()[0].Equals(InitialPosition));
	Preview->TickPlayback(1.1f);
	Bridge->TickTimeControlUI(0.2f);
	Bridge->RefreshRoutePlaybackUI();
	TestTrue(TEXT("Route completed"), Preview->GetPlaybackState() == ERoutePreviewPlaybackState::Completed);
	TestFalse(TEXT("Duration stops at completion"), Bridge->bTimeControlPerformanceActive);
	TestEqual(TEXT("Final elapsed duration"), Elapsed->GetText().ToString(), FString(TEXT("00:00:02")));
	TestEqual(TEXT("Bottom progress completes"), Bridge->RouteTimelineSlider->GetValue(), 1.0f);
	TestTrue(TEXT("Another preview starts"), Bridge->BeginScheduledRoutePreview(Error));
	Bridge->HandleRoutePauseClicked();
	Bridge->TickTimeControlUI(0.2f);
	TestFalse(TEXT("Manual pause detaches scheduled presentation"), Bridge->bTimeControlPerformanceActive);
	Preview->PreparedRouteData = Fixture;
	TestTrue(TEXT("Replacement import"), Preview->CommitPreparedRouteForWorkflow(31.9, 118.6, 30.0, false, Error));
	TestFalse(TEXT("Cannot silently play a replacement dataset"), Bridge->BeginScheduledRoutePreview(Error));
	return true;
}

#endif
