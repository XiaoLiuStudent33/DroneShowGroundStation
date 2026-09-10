#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameMapsSettings.h"
#include "GCSGameMode.h"
#include "GCSInvisiblePawn.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSGameModePawnContractTest,
	"DroneGCS.UI.VisualCleanup.GameModePawnContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSGameModePawnContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TestEqual(
		TEXT("Project global default GameMode is AGCSGameMode"),
		UGameMapsSettings::GetGlobalDefaultGameMode(),
		FString(TEXT("/Script/DroneShowPlatform1.GCSGameMode")));

	const AGCSGameMode* GameModeCDO = GetDefault<AGCSGameMode>();
	if (!TestNotNull(TEXT("AGCSGameMode CDO exists"), GameModeCDO))
	{
		return false;
	}

	TestEqual(
		TEXT("AGCSGameMode selects AGCSInvisiblePawn"),
		GameModeCDO->DefaultPawnClass.Get(),
		AGCSInvisiblePawn::StaticClass());

	const AGCSInvisiblePawn* PawnCDO = GetDefault<AGCSInvisiblePawn>();
	if (!TestNotNull(TEXT("AGCSInvisiblePawn CDO exists"), PawnCDO))
	{
		return false;
	}

	const UStaticMeshComponent* Mesh = PawnCDO->GetMeshComponent();
	TestNotNull(TEXT("Inherited DefaultPawn mesh remains present"), Mesh);
	if (Mesh)
	{
		TestFalse(TEXT("Inherited mesh visibility is disabled"), Mesh->IsVisible());
		TestTrue(TEXT("Inherited mesh is hidden in game"), Mesh->bHiddenInGame);
	}

	TestNotNull(TEXT("Inherited collision component remains present"), PawnCDO->GetCollisionComponent());
	TestNotNull(TEXT("Inherited movement component remains present"), PawnCDO->GetMovementComponent());
	TestFalse(TEXT("Default movement/input bindings disabled for orbit"), PawnCDO->bAddDefaultMovementBindings);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSInvisiblePawnRuntimeContractTest,
	"DroneGCS.UI.VisualCleanup.PossessionAndCameraViewTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSInvisiblePawnRuntimeContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}

	GameInstance->InitializeStandalone(TEXT("GCSInvisiblePawnRuntimeTestWorld"));
	UWorld* World = GameInstance->GetWorld();
	APlayerController* PlayerController = World
		? World->SpawnActor<APlayerController>()
		: nullptr;
	AGCSInvisiblePawn* Pawn = World
		? World->SpawnActor<AGCSInvisiblePawn>()
		: nullptr;
	ACameraActor* Camera = World
		? World->SpawnActor<ACameraActor>()
		: nullptr;

	if (!TestNotNull(TEXT("Test world created"), World)
		|| !TestNotNull(TEXT("PlayerController spawned"), PlayerController)
		|| !TestNotNull(TEXT("Invisible pawn spawned"), Pawn)
		|| !TestNotNull(TEXT("CameraActor spawned"), Camera))
	{
		GameInstance->Shutdown();
		return false;
	}

	PlayerController->Possess(Pawn);
	TestTrue(TEXT("PlayerController possesses invisible pawn"), PlayerController->GetPawn() == Pawn);
	TestTrue(TEXT("Pawn retains its controller"), Pawn->GetController() == PlayerController);
	TestNotNull(TEXT("Runtime movement component remains present"), Pawn->GetMovementComponent());
	TestNotNull(TEXT("Runtime collision component remains present"), Pawn->GetCollisionComponent());

	UStaticMeshComponent* Mesh = Pawn->GetMeshComponent();
	TestNotNull(TEXT("Runtime mesh component remains present"), Mesh);
	if (Mesh)
	{
		TestFalse(TEXT("Runtime pawn mesh is not visible"), Mesh->IsVisible());
		TestTrue(TEXT("Runtime pawn mesh is hidden in game"), Mesh->bHiddenInGame);
	}

	PlayerController->SpawnPlayerCameraManager();
	TestNotNull(
		TEXT("PlayerCameraManager is available for view-target switching"),
		PlayerController->PlayerCameraManager.Get());
	PlayerController->SetViewTargetWithBlend(Camera, 0.0f);
	TestTrue(TEXT("CameraActor can remain the view target"), PlayerController->GetViewTarget() == Camera);
	TestTrue(
		TEXT("ViewTarget switch does not change pawn possession"),
		PlayerController->GetPawn() == Pawn);

	// A tilted local basis must keep its center fixed through orbit and zoom.
 const FVector Center(1200, -5300, 800);
 const FVector Up = FVector(0, 1, 1).GetSafeNormal();
 TestTrue(TEXT("Accept geographic viewing basis"), Pawn->SetOrbitBasis(Center, FVector::XAxisVector, Up));
 const auto* Orbit = Pawn->FindComponentByClass<UCameraComponent>();
 TestNotNull(TEXT("Orbit camera exists"), Orbit);
 if (Orbit)
 {
  Pawn->RotateOrbit(75, 12);
  const double BeforeZoom = FVector::Distance(Orbit->GetComponentLocation(), Center);
  Pawn->ZoomOrbit(1);
  TestTrue(TEXT("Zoom approaches fixed center"), FVector::Distance(Orbit->GetComponentLocation(), Center) < BeforeZoom);
  TestTrue(TEXT("Camera looks at fixed center"), FVector::DotProduct(Orbit->GetForwardVector(), (Center - Orbit->GetComponentLocation()).GetSafeNormal()) > 0.999);
  const FVector BeforeTurn = Orbit->GetComponentLocation();
  const FQuat BeforeRotation = Orbit->GetComponentQuat();
  for (int32 I = 0; I < 360; ++I)
  {
   const FQuat Previous = Orbit->GetComponentQuat();
   Pawn->RotateOrbit(0, 1);
   TestTrue(TEXT("Pole crossing remains continuous"), Previous.AngularDistance(Orbit->GetComponentQuat()) < 0.03);
  }
  TestTrue(TEXT("Full vertical revolution restores position"), Orbit->GetComponentLocation().Equals(BeforeTurn, 0.1));
  TestTrue(TEXT("Full vertical revolution restores orientation"), BeforeRotation.AngularDistance(Orbit->GetComponentQuat()) < 0.001);
  Pawn->RotateOrbit(360, 0);
  TestTrue(TEXT("Full horizontal revolution restores position"), Orbit->GetComponentLocation().Equals(BeforeTurn, 0.1));
  Pawn->RotateOrbit(0, -90);
  TestTrue(TEXT("Can orbit below local ground plane"), FVector::DotProduct(Orbit->GetComponentLocation() - Center, Up) < 0);
  Pawn->RotateOrbit(0, 90);
  const FVector BeforePan = Orbit->GetComponentLocation();
  Pawn->PanOrbit(100, 200);
  TestFalse(TEXT("Locked pan moves camera framing"), Orbit->GetComponentLocation().Equals(BeforePan));
  TestEqual(TEXT("Locked pan preserves rotation origin"), Pawn->GetOrbitCenter(), Center);
  const FVector BeforeUnlock = Orbit->GetComponentLocation();
  Pawn->SetOrbitOriginLocked(false);
  TestTrue(TEXT("Unlock preserves current framing"), Orbit->GetComponentLocation().Equals(BeforeUnlock, 0.01));
  Pawn->PanOrbit(100, 200);
  TestFalse(TEXT("Unlocked view can pan"), Pawn->GetOrbitCenter().Equals(Center));
  Pawn->SetOrbitOriginLocked(true);
  TestEqual(TEXT("Relock restores saved center"), Pawn->GetOrbitCenter(), Center);
  TestFalse(TEXT("Reject degenerate basis"), Pawn->SetOrbitBasis(FVector::ZeroVector, Up, Up));
  TestEqual(TEXT("Invalid basis preserves center"), Pawn->GetOrbitCenter(), Center);
  TestTrue(TEXT("View operations do not steal external topdown camera"), PlayerController->GetViewTarget() == Camera);
 }
 PlayerController->UnPossess();
	Camera->Destroy();
	Pawn->Destroy();
	PlayerController->Destroy();
	GameInstance->Shutdown();
	return true;
}

#endif
