#pragma once
#include "CoreMinimal.h"
#include "DroneData.h"

class APlayerController;
class UFleetManagerSubsystem;
class UGCSUIBridgeSubsystem;
class ADroneFleetVisualizationActor;

// View adapter only: Fleet and the existing bridge remain the state authorities.
class FGCSWorkbenchModel
{
public:
    explicit FGCSWorkbenchModel(APlayerController* InController);
    void Refresh();
    void SetSearch(const FString& Value);
    void SetChecked(const FString& ID, bool Checked);
    void SelectFiltered(bool Checked);
    void Inspect(const FString& ID);
    bool IsChecked(const FString& ID) const { return CheckedIDs.Contains(ID); }
    bool CanSelect() const;
    FString DetailID() const;
    FString DetailText() const;
    FString LinkText() const;
    bool Connected() const;
    bool Locate();
    bool Pick(FVector2D ViewportPoint, float Radius);
    bool SceneFrame(FVector& Origin, FVector& East, FVector& North, FVector& Up) const;
    UFleetManagerSubsystem* Fleet() const;
    UGCSUIBridgeSubsystem* Bridge() const;
    ADroneFleetVisualizationActor* Visualization() const;
    TArray<TSharedPtr<FDroneData>> Filtered;
    int32 OnlineCount=0;
    int32 SelectedCount() const { return CheckedIDs.Num(); }
    int32 ListGeneration=0;
private:
    void Filter();
    TWeakObjectPtr<APlayerController> Controller;
    TArray<TSharedPtr<FDroneData>> Rows;
    TSet<FString> CheckedIDs;
    FString Search, Session;
    int64 Revision=MIN_int64;
};
