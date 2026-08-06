#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GCSDroneRowView.generated.h"

class UBorder;
class UUserWidget;

/**
 * Presentation state owned by one authored FleetPanel row.
 *
 * The SelectedOverlay Border remains an editable UMG asset component. The UI
 * bridge supplies only row data and the current detail DroneID; this object is
 * solely responsible for translating that ID into its own selected visual.
 */
UCLASS(BlueprintType)
class DRONESHOWPLATFORM1_API UGCSDroneRowView final : public UObject
{
	GENERATED_BODY()

public:
	bool Initialize(UUserWidget* InFleetPanel, int32 RowIndex);
	void SetDroneID(const FString& InDroneID);
	void NotifyCurrentSelectedDroneID(const FString& CurrentSelectedDroneID);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Fleet Row")
	void SetSelectedVisual(bool bSelected);

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet Row")
	bool IsSelectedVisual() const { return bSelectedVisual; }

private:
	UPROPERTY(Transient)
	TObjectPtr<UBorder> SelectedOverlay;

	FString DroneID;
	bool bSelectedVisual = false;
};
