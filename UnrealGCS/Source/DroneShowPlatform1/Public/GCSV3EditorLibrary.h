#pragma once
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GCSV3EditorLibrary.generated.h"

UCLASS()
class DRONESHOWPLATFORM1_API UGCSV3EditorLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="GCS|Editor")
    static bool BuildWorkbench();
    UFUNCTION(BlueprintCallable, Category="GCS|Editor")
    static FString InspectWidgets(const FString& AssetPath);
};
