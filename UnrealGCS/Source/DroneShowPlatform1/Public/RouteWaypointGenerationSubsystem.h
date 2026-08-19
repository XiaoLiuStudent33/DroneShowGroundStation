#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "RouteArtifactManifestV2.h"
#include "RouteWaypointBatchGenerator.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "RouteWaypointGenerationSubsystem.generated.h"

struct FRouteWaypointFinalizeAsyncResult
{
	bool bSuccess = false;
	FRouteArtifactManifestV2 Manifest;
	FString Error;
};

/** GameThread authority coordinator for one complete Route Artifact generation. */
UCLASS()
class DRONESHOWPLATFORM1_API URouteWaypointGenerationSubsystem final
	: public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** WorkerCountOverride=0 selects the C2B-profiled bounded runtime default. */
	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Artifact Generation")
	bool StartGeneration(int32 WorkerCountOverride, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Artifact Generation")
	bool CancelGeneration(FString& OutError);

	bool IsGenerationActive() const;
	FRouteWaypointBatchProgress GetProgress() const;
	const FRouteArtifactManifestV2& GetLastGeneratedManifest() const
	{
		return LastGeneratedManifest;
	}

private:
	bool TickGeneration(float DeltaSeconds);
	bool BuildStartInputs(
		int32 WorkerCountOverride,
		FRouteWaypointBatchRequest& OutRequest,
		FRouteArtifactManifestV2& OutManifestDraft,
		FString& OutError);
	void StopCompletionTicker();

	TUniquePtr<FRouteWaypointBatchGenerator> ActiveBatch;
	FRouteWaypointBatchProgress LastProgress;
	FRouteArtifactManifestV2 PendingManifestDraft;
	FRouteWaypointSourceValidationContext PendingSourceValidationContext;
	FRouteArtifactManifestV2 LastGeneratedManifest;
	FTSTicker::FDelegateHandle CompletionTickerHandle;
	TFuture<FRouteWaypointFinalizeAsyncResult> FinalizeFuture;
	bool bFinalizeStarted = false;
	bool bDeinitializing = false;
};
