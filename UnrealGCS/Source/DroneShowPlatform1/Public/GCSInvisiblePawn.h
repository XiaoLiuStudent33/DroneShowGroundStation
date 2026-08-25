// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "GCSInvisiblePawn.generated.h"

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
};
