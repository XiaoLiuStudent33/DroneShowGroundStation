/**
 * @brief UE Fleet 状态权威源，维护 DronesByID、稳定 DroneOrder 和详情选择。
 *
 * 分类：Production。
 * 输入：GCSServerAdapter 原子提交的 Snapshot/Delta；输出：UI 和 Fleet Visualization 事件/查询。
 * 关键约束：CurrentSelectedDroneID 只表示详情选择；DroneOrder 是 ParticleIndex 稳定映射基础。
 */
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
 * Immutable-after-initialization geographic anchor for one fleet session.
 *
 * Phase 2 only stores the geodetic reference. A later visualization phase must
 * convert Longitude/Latitude/Height through CesiumGeoreference; the legacy
 * flat-earth longitude/latitude delta formula must not be reintroduced.
 * Until this origin exists every drone visual stays at FVector::ZeroVector;
 * an individual drone with invalid GPS likewise uses FVector::ZeroVector.
 */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FFleetGeoOrigin
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone GCS|Fleet|Geo")
	bool bOriginInitialized = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone GCS|Fleet|Geo")
	FString OriginDroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone GCS|Fleet|Geo")
	double OriginLatitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone GCS|Fleet|Geo")
	double OriginLongitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone GCS|Fleet|Geo")
	float OriginAltitudeAMSL = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone GCS|Fleet|Geo")
	bool bOriginAltitudeValid = false;
};

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

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Fleet|Geo")
	FFleetGeoOrigin GetFleetGeoOrigin() const { return FleetGeoOrigin; }

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FFleetChangedEvent OnFleetChanged;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FDroneDataUpdatedEvent OnDroneUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FDroneSelectedEvent OnDroneSelected;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|Fleet")
	FFleetDataBatchAppliedEvent OnDataBatchApplied;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FDSPGCSTelemetryChunkAssemblyTest;
	int32 DataBatchAppliedBroadcastCountForTests = 0;
#endif

	UPROPERTY()
	TMap<FString, FDroneData> DronesByID;

	UPROPERTY()
	TArray<FString> DroneOrder;

	UPROPERTY()
	FString CurrentSelectedDroneID;

	UPROPERTY()
	int64 FleetRevision = 0;

	UPROPERTY()
	FFleetGeoOrigin FleetGeoOrigin;

	UPROPERTY()
	FString ActiveFleetSessionID;

	void TryInitializeGeoOrigin();
	void ResetGeoOrigin();
};
