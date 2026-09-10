#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/StaticMeshComponent.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/Script.h"
#include "GameFramework/PlayerController.h"
#include "GCSInvisiblePawn.h"
#include "GCSUIBridgeSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSOrbitControlsTest, "DroneGCS.UI.Orbit.ControlsLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSOrbitControlsTest::RunTest(const FString&)
{
 auto* GI = NewObject<UGameInstance>(GEngine);
 GI->InitializeStandalone(TEXT("OrbitControlsTest"));
 auto* PC = GI->GetWorld()->SpawnActor<APlayerController>();
 auto* LP = NewObject<ULocalPlayer>(GEngine);
 PC->SetPlayer(LP);
 GI->GetWorld()->AddController(PC); // Standalone editor test world has no game-mode registration.
 auto* Pawn = GI->GetWorld()->SpawnActor<AGCSInvisiblePawn>();
 PC->Possess(Pawn);
 UClass* PanelClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_GCS_RightPanel.WBP_GCS_RightPanel_C"));
 auto* Panel = PanelClass ? CreateWidget<UUserWidget>(PC, PanelClass) : nullptr;
 if (!TestNotNull(TEXT("User-authored panel loads"), Panel)) { GI->Shutdown(); return false; }
 Panel->SetPlayerContext(FLocalPlayerContext(LP, GI->GetWorld()));
 TestTrue(TEXT("Fixture has owning controller"), Panel->GetOwningPlayer() == PC);
 auto* Bridge = GI->GetSubsystem<UGCSUIBridgeSubsystem>();
 Bridge->MainWidgetRef = Panel;
 Bridge->RightPanelWidget = Panel;
 auto* Button = Cast<UButton>(Panel->WidgetTree->FindWidget(TEXT("BTN_ResetOrbitView")));
 auto* Check = Cast<UCheckBox>(Panel->WidgetTree->FindWidget(TEXT("CHK_OrbitOriginLocked")));
 if (!TestNotNull(TEXT("Reset button authored"), Button) || !TestNotNull(TEXT("Lock checkbox authored"), Check)) { GI->Shutdown(); return false; }
 Pawn->SetOrbitOriginLocked(false);
 Pawn->PanOrbit(500, 300);
 const FVector Panned = Pawn->GetOrbitCenter();
 Bridge->BindOrbitControls();
 Bridge->BindOrbitControls();
 TestFalse(TEXT("Binding projects unlocked state"), Check->IsChecked());
 TestEqual(TEXT("Rebind preserves view center"), Pawn->GetOrbitCenter(), Panned);
 Check->OnCheckStateChanged.Broadcast(true);
 TestTrue(TEXT("Checkbox locks pawn"), Pawn->IsOrbitOriginLocked());
 Check->OnCheckStateChanged.Broadcast(false);
 TestFalse(TEXT("Checkbox unlocks pawn"), Pawn->IsOrbitOriginLocked());
 Pawn->ResetOrbitView(); // Same operation as Home.
 Bridge->SyncOrbitControls();
 TestTrue(TEXT("Home reset reflected in checkbox"), Check->IsChecked());
 Pawn->SetOrbitOriginLocked(false);
 Button->OnClicked.Broadcast();
 TestTrue(TEXT("Button resets lock"), Pawn->IsOrbitOriginLocked());
 Bridge->UnbindOrbitControls();
 Pawn->SetOrbitOriginLocked(false);
 Button->OnClicked.Broadcast();
 Check->OnCheckStateChanged.Broadcast(true);
 TestFalse(TEXT("Unbound widgets cannot change camera"), Pawn->IsOrbitOriginLocked());
 Bridge->BindOrbitControls();
 TestFalse(TEXT("Reopen adopts current state"), Check->IsChecked());
 Bridge->bTopDownLocateLocked = true;
 Bridge->SyncOrbitControls();
 TestFalse(TEXT("Topdown disables perspective lock control"), Check->GetIsEnabled());
 Bridge->bTopDownLocateLocked = false;
 PC->UnPossess();
 Bridge->SyncOrbitControls();
 TestFalse(TEXT("No pawn disables reset"), Button->GetIsEnabled());
 Bridge->UnbindOrbitControls();
 Bridge->MainWidgetRef.Reset(); // Use this fixture's standalone world, not the widget editor world.
 auto* AxesClass = LoadClass<AActor>(nullptr, TEXT("/Game/GCS/Visualization/Coordinate/BP_GCS_CoordinateAxes.BP_GCS_CoordinateAxes_C"));
 if (TestNotNull(TEXT("Actual axes blueprint available"), AxesClass))
 {
  FEditorScriptExecutionGuard ScriptGuard; // Allow actual blueprint execution in the editor test world.
  AActor* Axes = nullptr;
  for (TActorIterator<AActor> It(GI->GetWorld()); It; ++It) { if (It->FindFunction(TEXT("InitializeCoordinateAxes"))) { Axes = *It; break; } }
  if (!Axes) Axes = GI->GetWorld()->SpawnActor<AActor>(AxesClass);
  for (int Pass = 0; Pass < 2; ++Pass)
  {
   TestTrue(TEXT("Actual axes initialized"), Bridge->InitializeCoordinateAxes(FVector(0,0,15), FVector::XAxisVector, -FVector::YAxisVector, 0, 1000));
   TInlineComponentArray<UStaticMeshComponent*> Meshes(Axes);
   TestTrue(TEXT("Axes meshes generated"), Meshes.Num() >= 4);
   for (auto* Mesh : Meshes)
   {
    TestTrue(TEXT("Axis visual offset remains above display origin on repeated apply"), FMath::IsNearlyEqual(Mesh->GetComponentLocation().Z, 35.0, 0.01));
   }
   TestTrue(TEXT("East orientation preserved"), Axes->GetActorForwardVector().Equals(FVector::XAxisVector, 0.001));
   TestTrue(TEXT("North orientation preserved"), Axes->GetActorRightVector().Equals(-FVector::YAxisVector, 0.001));
   TestTrue(TEXT("Actor origin unchanged"), Axes->GetActorLocation().Equals(FVector(0,0,15), 0.001));
  }
 }
 Bridge->MainWidgetRef.Reset();
 Bridge->RightPanelWidget.Reset();
 GI->Shutdown();
 return true;
}
#endif
