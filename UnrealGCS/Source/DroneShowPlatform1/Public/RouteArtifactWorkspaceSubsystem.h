#pragma once

#include "CoreMinimal.h"
#include "RouteArtifactWorkspaceManager.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "RouteArtifactWorkspaceSubsystem.generated.h"

/** GameInstance lifecycle and Authority orchestration for Route Artifact workspace IO. */
UCLASS()
class DRONESHOWPLATFORM1_API URouteArtifactWorkspaceSubsystem final
	: public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Artifact Workspace")
	bool SetConfiguredWorkspaceRoot(const FString& ConfiguredRoot, FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Route Artifact Workspace")
	FString GetConfiguredWorkspaceRoot() const;

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Route Artifact Workspace")
	bool ResolveWorkspaceRoot(FString& OutRoot, FString& OutError) const;
	bool GetWorkspaceVolumeFreeBytes(int64& OutFreeBytes, FString& OutError) const;
	bool RecoverWorkspace(FRouteArtifactRecoveryResult& OutResult, FString& OutError);

	bool BeginGeneration(
		const FRouteArtifactBuildIdentity& BuildIdentity,
		FRouteArtifactGenerationHandle& OutHandle,
		FString& OutError);
	bool ResolveSafeStagingRoutePath(
		int32 RouteSlotIndex,
		FString& OutPath,
		FString& OutError) const;
	bool FinalizeGeneration(const FRouteArtifactManifest& Manifest, FString& OutError);
	bool FinalizeGeneratedRoutes(
		FRouteArtifactManifest& InOutManifest,
		const FRouteWaypointSourceValidationContext& SourceValidationContext,
		FString& OutError);
	bool AbortGeneration(FString& OutError);
	bool LoadAndValidateCurrentArtifact(FRouteArtifactManifest& OutManifest, FString& OutError);

	ERouteArtifactCurrentStatus GetCurrentStatus() const;
	ERouteArtifactGenerationStatus GetGenerationStatus() const;
	bool IsCurrentSwapInProgress() const;
	const FRouteArtifactCurrentInfo& GetCurrentInfo() const;
	const FRouteArtifactRecoveryResult& GetLastRecoveryResult() const;
	const FRouteArtifactSourceIdentity* GetCurrentSourceIdentity() const;
	bool FindCurrentRouteRecord(
		int32 RouteSlotIndex,
		FRouteArtifactRecord& OutRecord) const;

private:
	friend class URouteWaypointGenerationSubsystem;
	FRouteArtifactWorkspaceManager* GetGenerationManager() const { return Manager.Get(); }
	bool CaptureAuthorityGate(
		FRouteArtifactWorkspaceAuthorityGate& OutGate,
		FString& OutError) const;

	TUniquePtr<FRouteArtifactWorkspaceManager> Manager;
};
