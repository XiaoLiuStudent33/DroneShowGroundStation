// Copyright Epic Games, Inc. All Rights Reserved.

#include "GCSInvisiblePawn.h"

#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GCSUIBridgeSubsystem.h"
#include "InputCoreTypes.h"

AGCSInvisiblePawn::AGCSInvisiblePawn()
{
	if (UStaticMeshComponent* PawnMesh = GetMeshComponent())
	{
		PawnMesh->SetVisibility(false, true);
		PawnMesh->SetHiddenInGame(true, true);
	}
}

void AGCSInvisiblePawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (!PlayerInputComponent)
	{
		return;
	}

	// ADefaultPawn binds raw Mouse X/Y directly to controller rotation. Replace
	// only those two bindings; movement and gamepad look remain unchanged.
	PlayerInputComponent->RemoveAxisBinding(FName(TEXT("DefaultPawn_Turn")));
	PlayerInputComponent->RemoveAxisBinding(FName(TEXT("DefaultPawn_LookUp")));
	PlayerInputComponent->BindAxis(
		TEXT("DefaultPawn_Turn"), this, &AGCSInvisiblePawn::HandleMouseTurn);
	PlayerInputComponent->BindAxis(
		TEXT("DefaultPawn_LookUp"), this, &AGCSInvisiblePawn::HandleMouseLookUp);
}

void AGCSInvisiblePawn::HandleMouseTurn(const float Value)
{
	if (!FMath::IsNearlyZero(Value) && IsMouseLookAllowed())
	{
		AddControllerYawInput(Value);
	}
}

void AGCSInvisiblePawn::HandleMouseLookUp(const float Value)
{
	if (!FMath::IsNearlyZero(Value) && IsMouseLookAllowed())
	{
		AddControllerPitchInput(Value);
	}
}

bool AGCSInvisiblePawn::IsMouseLookAllowed() const
{
	const APlayerController* PlayerController = Cast<APlayerController>(GetController());
	if (!PlayerController || !PlayerController->IsInputKeyDown(EKeys::RightMouseButton))
	{
		return false;
	}

	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UGCSUIBridgeSubsystem* UIBridge = GameInstance
		? GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>()
		: nullptr;
	if (!UIBridge || !FSlateApplication::IsInitialized())
	{
		return false;
	}

	// Full-screen UMG roots can remain in the Slate hit path over visually empty
	// scene space. Only authored, visible GCS panels should block camera look.
	return !UIBridge->IsPointerOverBlockingGCSUI(
		FSlateApplication::Get().GetCursorPos());
}
