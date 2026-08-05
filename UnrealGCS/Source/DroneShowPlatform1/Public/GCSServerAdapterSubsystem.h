#pragma once

#include "CoreMinimal.h"
#include "GCSDataTypes.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GCSServerAdapterSubsystem.generated.h"

class IGCSDataProvider;

/**
 * Protocol-neutral application port between data providers and FleetManager.
 * It never creates sockets and never parses MAVLink.
 */
UCLASS()
class DRONESHOWPLATFORM1_API UGCSServerAdapterSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool RegisterProvider(IGCSDataProvider& Provider);
	void UnregisterProvider(IGCSDataProvider& Provider);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Data")
	bool PushFleetSnapshot(const FGCSFleetSnapshot& Snapshot);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Data")
	bool PushTelemetryBatch(const FGCSTelemetryBatch& Batch);

private:
	void ScheduleFlush();
	void FlushPendingData();
	bool ValidateHeader(const FGCSMessageHeader& Header) const;

	FCriticalSection PendingDataMutex;
	TOptional<FGCSFleetSnapshot> PendingSnapshot;
	TMap<FString, FDroneDataDelta> PendingDeltasByID;
	TArray<FString> PendingDeltaOrder;
	FGCSMessageHeader PendingDeltaHeader;
	int64 PendingDeltaFleetRevision = 0;
	bool bFlushScheduled = false;

	TArray<IGCSDataProvider*> Providers;

	static constexpr int32 MaxPendingDroneDeltas = 20000;
};
