// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "GCSGameMode.generated.h"

/** Game mode that keeps the engine controller behavior and uses the invisible project pawn. */
UCLASS()
class DRONESHOWPLATFORM1_API AGCSGameMode final : public AGameMode
{
	GENERATED_BODY()

public:
	AGCSGameMode();
};
