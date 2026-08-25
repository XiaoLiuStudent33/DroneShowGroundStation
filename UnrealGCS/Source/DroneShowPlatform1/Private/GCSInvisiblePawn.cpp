// Copyright Epic Games, Inc. All Rights Reserved.

#include "GCSInvisiblePawn.h"

#include "Components/StaticMeshComponent.h"

AGCSInvisiblePawn::AGCSInvisiblePawn()
{
	if (UStaticMeshComponent* PawnMesh = GetMeshComponent())
	{
		PawnMesh->SetVisibility(false, true);
		PawnMesh->SetHiddenInGame(true, true);
	}
}
