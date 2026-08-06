#pragma once

#include "CoreMinimal.h"
#include "DroneData.h"
#include "GCSDataTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FleetManagerSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFleetChangedEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FDroneDataUpdatedEvent,
	const FDroneData&,
	DroneData);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FDroneSelectedEvent,
	const FString&,
	DroneID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FFleetDataBatchAppliedEvent,
	int64,
	FleetRevision,
	int32,
	UpdatedDroneCount);

/**
 * Authoritative UE-side collection of UI drone snapshots.
 *
 * Widgets never own drone state. They query this subsystem and react to its
 * events. Stage 1 seeds mock records; a later adapter can call the same update
 * API with server data without changing any Widget Blueprint.
 */
UCLASS()
class DRONESHOWPLATFORM1_API UFleetManagerSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Fleet")
	bool AddDrone(const FDroneData& DroneData);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Fleet")
	bool RemoveDrone(const FString& DroneID);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Fleet")
	bool UpdateDrone(const FDroneData& DroneData);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Fleet")
	bool SelectDrone(const FString& DroneID);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Fleet")
	bool ApplySnapshot(const FGCSFleetSnapshot& Snapshot);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Fleet")
	bool ApplyDeltaBatch(const FGCSTelemetryBatch& Batch);

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet")
	TArray<FDroneData> GetDronePage(int32 Offset, int32 Limit) const;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet")
	int32 GetDroneCount() const;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet")
	int64 GetFleetRevision() const { return FleetRevision; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet")
	bool GetDroneByID(const FString& DroneID, FDroneData& OutDroneData) const;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet")
	bool GetSelectedDrone(FDroneData& OutDroneData) const;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet")
	FString GetCurrentSelectedDroneID() const { return CurrentSelectedDroneID; }

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FFleetChangedEvent OnFleetChanged;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FDroneDataUpdatedEvent OnDroneUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FDroneSelectedEvent OnDroneSelected;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FFleetDataBatchAppliedEvent OnDataBatchApplied;

private:
	UPROPERTY()
	TMap<FString, FDroneData> DronesByID;

	UPROPERTY()
	TArray<FString> DroneOrder;

	UPROPERTY()
	FString CurrentSelectedDroneID;

	UPROPERTY()
	int64 FleetRevision = 0;
};
