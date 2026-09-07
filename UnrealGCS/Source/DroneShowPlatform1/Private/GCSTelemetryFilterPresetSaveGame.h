#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "GCSTelemetryFilterPresetSaveGame.generated.h"

USTRUCT()
struct FGCSTelemetryFilterPresetRule
{
	GENERATED_BODY()

	UPROPERTY() FName Field = NAME_None;
	UPROPERTY() FString Operator = TEXT("--");
	UPROPERTY() FString Input;
	UPROPERTY() bool bEnabled = true;
};

USTRUCT()
struct FGCSTelemetryFilterPresetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() int32 MatchModeIndex = 0;
	UPROPERTY() int32 SelectedConditionIndex = INDEX_NONE;
	UPROPERTY() TArray<FGCSTelemetryFilterPresetRule> Rules;
};

UCLASS()
class UGCSTelemetryFilterPresetSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 FormatVersion = 1;
	UPROPERTY() TArray<FGCSTelemetryFilterPresetRecord> Presets;
};
