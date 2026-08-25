#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Camera/CameraActor.h"
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
	TestTrue(TEXT("Default movement/input bindings remain enabled"), PawnCDO->bAddDefaultMovementBindings);
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

	PlayerController->UnPossess();
	Camera->Destroy();
	Pawn->Destroy();
	PlayerController->Destroy();
	GameInstance->Shutdown();
	return true;
}

#endif
