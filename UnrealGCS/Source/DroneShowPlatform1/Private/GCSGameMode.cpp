// Copyright Epic Games, Inc. All Rights Reserved.

#include "GCSGameMode.h"

#include "GCSInvisiblePawn.h"

AGCSGameMode::AGCSGameMode()
{
	DefaultPawnClass = AGCSInvisiblePawn::StaticClass();
}
