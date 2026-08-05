#pragma once

#include "CoreMinimal.h"
#include "GCSDataProvider.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MockDroneDataSubsystem.generated.h"

class UGCSServerAdapterSubsystem;

/**
 * Stage-only local data provider.
 *
 * It is intentionally separate from FleetManager. The future server adapter
 * will publish through the same FleetManager API and this provider can simply
 * be disabled in configuration.
 */
UCLASS()
class DRONESHOWPLATFORM1_API UMockDroneDataSubsystem final
	: public UGameInstanceSubsystem
	, public IGCSDataProvider
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual FName GetProviderID() const override;
	virtual bool Start(UGCSServerAdapterSubsystem& Adapter) override;
	virtual void Stop() override;

private:
	TWeakObjectPtr<UGCSServerAdapterSubsystem> AdapterRef;
};
