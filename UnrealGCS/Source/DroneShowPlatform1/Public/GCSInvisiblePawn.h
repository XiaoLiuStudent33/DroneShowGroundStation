// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "GCSInvisiblePawn.generated.h"

class UInputComponent;

/**
 * Project default pawn that preserves ADefaultPawn movement and input while
 * suppressing the engine sphere visualization.
 */
UCLASS()
class DRONESHOWPLATFORM1_API AGCSInvisiblePawn final : public ADefaultPawn
{
	GENERATED_BODY()

public:
	AGCSInvisiblePawn();

protected:
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	void HandleMouseTurn(float Value);
	void HandleMouseLookUp(float Value);
	bool IsMouseLookAllowed() const;
};
