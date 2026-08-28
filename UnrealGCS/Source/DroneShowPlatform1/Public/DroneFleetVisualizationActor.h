/**
 * @brief 将完整 Fleet 批量投影到 Cesium 世界坐标并推送一个 Niagara System。
 *
 * 分类：Production。
 * 输入：FleetManager 稳定 DroneOrder；输出：User.DronePositions 与稳定 ParticleIndex。
 * 关键约束：全 Fleet 只允许一个 Actor、一个 NiagaraComponent；禁止 per-drone Actor/Component/Tick。
 */
#pragma once

#include "CoreMinimal.h"
#include "DroneData.h"
#include "GameFramework/Actor.h"
#include "DroneFleetVisualizationActor.generated.h"

class ACesiumGeoreference;
class UFleetManagerSubsystem;
class UNiagaraComponent;
class UNiagaraSystem;
class USceneComponent;
struct FFleetGeoOrigin;

UENUM(BlueprintType)
enum class EGCSVisualizationMode : uint8
{
	Live,
	RoutePreview
};

/** Global renderer handoff state shared by Live and full RoutePreview. */
enum class EDronePrimaryDisplayMode : uint8
{
	Marker,
	Mesh
};

/**
 * Fleet 到 Niagara 的轻量数据桥。
 *
 * 整个机群只使用一个 Actor 和一个 NiagaraComponent，避免未来数千架无人机
 * 产生 per-drone UObject、Actor、Component 或 Tick 开销。
 */
UCLASS(BlueprintType)
class DRONESHOWPLATFORM1_API ADroneFleetVisualizationActor final : public AActor
{
	GENERATED_BODY()

public:
	ADroneFleetVisualizationActor();

	/** 立即按 Fleet 的稳定 DroneOrder 重建并批量推送 Niagara 数组。 */
	UFUNCTION(BlueprintCallable, Category = "Drone GCS|Visualization")
	void RefreshFromFleet();

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Visualization")
	int32 GetVisualizationDroneCount() const { return DronePositions.Num(); }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Visualization")
	TArray<FString> GetParticleDroneIDs() const { return ParticleDroneIDs; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Visualization")
	TArray<FVector> GetDronePositions() const { return DronePositions; }

#if WITH_DEV_AUTOMATION_TESTS
	TArray<float> GetDroneDisplayIDsForTest() const { return DroneDisplayIDs; }
#endif

	/** Updates the Niagara label highlight from the existing command selection authority. */
	void SetCommandSelectedDroneIDs(const TArray<FString>& DroneIDs);

	/** Updates only the stable particles identified by DroneID and preserves every other color. */
	void SetDroneColorsForIDs(const TArray<FString>& DroneIDs, const FLinearColor& Color);

	/** Display-only zero-based RouteSlot override keyed by the unchanged network DroneID. */
	void SetConfirmedRouteSlotDisplayOverrides(const TMap<FString, int32>& OverridesByDroneID);
	void ClearConfirmedRouteSlotDisplayOverrides();

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Visualization|Geo")
	bool IsOriginWorldPositionInitialized() const { return bOriginWorldPositionInitialized; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Visualization|Geo")
	FVector GetOriginWorldPosition() const { return OriginWorldPosition; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Visualization")
	UNiagaraComponent* GetDroneFleetNiagara() const { return DroneFleetNiagara; }

	/** Enables the existing ID sprite renderer through its global Niagara user parameter. */
	void SetDroneLabelsVisible(bool bVisible);

	/**
	 * Converts one RouteSlot-ordered Local ENU position array through the same
	 * Cesium ShowOrigin path used by full route preview playback.
	 */
	bool ConvertRouteENUPositionsToWorld(
		const TArray<FVector3f>& PositionsENU,
		double OriginLatitude,
		double OriginLongitude,
		double OriginAltitude,
		TArray<FVector>& OutWorldPositions,
		FVector& OutRouteOriginWorld,
		FString& OutError,
		FVector* OutWorldBoundsCenter = nullptr);

	/** Submits RouteSlotIndex-ordered world positions to the independent overlay channel. */
	void SetRouteTargetPositions(const TArray<FVector>& Positions);

	/** Changes only the route-target renderer visibility user parameter. */
	void SetRouteTargetsVisible(bool bVisible);

	/** Clears all route-target render data without touching live drone arrays. */
	void ClearRouteTargets();

	int32 GetRouteTargetCount() const { return RouteTargetPositions.Num(); }
	bool AreRouteTargetsVisible() const { return bRouteTargetsVisible; }

	/**
	 * Replaces only the visualization source with one route frame. Route slots
	 * are preview identities and are never inserted into FleetManager.
	 */
	bool SetRoutePreviewFrame(
		const TArray<FVector3f>& PositionsENU,
		const TArray<FLinearColor>& Colors,
		double OriginLatitude,
		double OriginLongitude,
		double OriginAltitude,
		FString& OutError);

	/** Restores the newest FleetManager snapshot without changing Fleet state. */
	void ExitRoutePreview();

	UFUNCTION(BlueprintPure, Category = "Drone GCS|Visualization")
	EGCSVisualizationMode GetVisualizationMode() const { return VisualizationMode; }

	/** Prints one O(1) snapshot of the shared Flight Marker renderer state. */
	void LogFlightMarkerStatus() const;

	/**
	 * Returns the Cesium local tangent basis used by the current visualization
	 * source. Route preview uses its ShowOrigin; Live uses the Fleet geo origin.
	 */
	bool TryGetGeographicTangentBasis(
		FVector& OutOriginWorld,
		FVector& OutEastWorld,
		FVector& OutNorthWorld,
		FVector& OutUpWorld) const;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Drone GCS|Visualization")
	TObjectPtr<USceneComponent> SceneRoot;

	/** 唯一的机群渲染组件；禁止为每架无人机创建组件。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone GCS|Visualization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UNiagaraComponent> DroneFleetNiagara;

	/** 在编辑器中指定 NS_DroneFleet，不在代码中硬编码 Content 路径。 */
	UPROPERTY(EditAnywhere, Category = "Drone GCS|Visualization")
	TObjectPtr<UNiagaraSystem> DroneFleetSystem;

	UPROPERTY(Transient)
	TArray<FString> ParticleDroneIDs;

	UPROPERTY(Transient)
	TArray<FVector> DronePositions;

	/** Persistent preview colors in the same stable particle order as ParticleDroneIDs. */
	UPROPERTY(Transient)
	TArray<FLinearColor> DroneColors;

	/** Per-particle display identifiers: confirmed zero-based RouteSlot or stable particle index. */
	UPROPERTY(Transient)
	TArray<float> DroneDisplayIDs;

	/** Per-particle command-selection mask; derived from GCSUIBridgeSubsystem selection authority. */
	UPROPERTY(Transient)
	TArray<float> DroneSelectedMasks;

	/** Frame-0 RouteSlot-ordered world positions; independent from User.DronePositions. */
	UPROPERTY(Transient)
	TArray<FVector> RouteTargetPositions;

	/** Derived render cache only; GCSUIBridgeSubsystem remains selection authority. */
	TSet<FString> CommandSelectedDroneIDs;
	/** Display-only cache; keys are real DroneIDs and values are zero-based RouteSlots. */
	TMap<FString, int32> ConfirmedRouteSlotDisplayOverrides;
	EGCSVisualizationMode VisualizationMode = EGCSVisualizationMode::Live;

	/**
	 * Live render-only state saved while RoutePreview owns the Niagara arrays.
	 * Fleet telemetry remains authoritative in FleetManager throughout preview.
	 */
	TArray<FString> SuspendedLiveParticleDroneIDs;
	TArray<FLinearColor> SuspendedLiveDroneColors;
	bool bDroneLabelAttributesDirty = true;
	bool bDroneColorsDirty = true;
	bool bRouteTargetsVisible = false;
	EDronePrimaryDisplayMode DronePrimaryDisplayMode = EDronePrimaryDisplayMode::Marker;
	bool bDroneRendererVisibilityInitialized = false;
	bool bDroneMeshVisible = false;
	bool bLiveDroneHaloVisible = false;

	/** 仅粒子数量变化时重建系统；普通遥测更新只批量刷新数组。 */
	int32 LastPushedDroneCount = INDEX_NONE;
	int32 LastPushedRouteTargetCount = INDEX_NONE;
	int32 LastLoggedFleetCount = INDEX_NONE;
	int32 LastLoggedParticleCount = INDEX_NONE;

	TWeakObjectPtr<UFleetManagerSubsystem> FleetManager;
	TWeakObjectPtr<ACesiumGeoreference> CesiumGeoreference;

	bool bCesiumLookupAttempted = false;
	bool bCesiumReadyLogged = false;
	bool bCesiumMissingLogged = false;
	bool bOriginWorldPositionInitialized = false;
	FVector OriginWorldPosition = FVector::ZeroVector;
	bool bRoutePreviewOriginWorldPositionInitialized = false;
	FVector RoutePreviewOriginWorldPosition = FVector::ZeroVector;
	bool bCachedFleetVisualCenterValid = false;
	FVector CachedFleetVisualCenter = FVector::ZeroVector;
	FString CachedOriginDroneID;
	double CachedOriginLatitude = 0.0;
	double CachedOriginLongitude = 0.0;
	float CachedOriginAltitudeAMSL = 0.0f;
	bool bCachedOriginAltitudeValid = false;
	bool bAdaptiveFlightMarkerSizeInitialized = false;
	float CurrentAdaptiveFlightMarkerSize = 32.0f;
	float LastSubmittedFlightMarkerSize = -1.0f;
	bool bLastFlightMarkerCalculationValid = false;
	bool bLastFlightMarkerCalculationOrthographic = false;
	int32 LastFlightMarkerViewportWidth = 0;
	int32 LastFlightMarkerViewportHeight = 0;
	double LastFlightMarkerCameraDistance = 0.0;
	float LastFlightMarkerFOV = 0.0f;
	float LastFlightMarkerOrthoWidth = 0.0f;
	float LastRawFlightMarkerSize = 0.0f;
	float LastClampedFlightMarkerSize = 0.0f;
	bool bLastSphereProjectedDiameterValid = false;
	float LastSphereProjectedDiameterPixels = 0.0f;

	void BindFleetManager();
	void UnbindFleetManager();
	void CacheCesiumGeoreference();
	void UpdateOriginWorldPosition(const FFleetGeoOrigin& FleetGeoOrigin);
	FVector ConvertDroneGPSPosition(
		const FDroneData& Drone,
		const FFleetGeoOrigin& FleetGeoOrigin) const;
	void RebuildDroneLabelAttributes();
	void UpdatePrimaryDroneRendererVisibility();
	void SetPrimaryDroneDisplayMode(EDronePrimaryDisplayMode NewDisplayMode);
	bool UpdatePrimaryDroneDisplayModeForProjectedDiameter(double ProjectedDiameterPixels);
	void UpdateAdaptiveFlightMarkerSize(float DeltaSeconds);
	void PushDroneLabelAttributesToNiagara();
	bool PushDroneColorsToNiagara();
	void PushArraysToNiagara();
	void PushRouteTargetsToNiagara();

	UFUNCTION()
	void HandleFleetChanged();

	UFUNCTION()
	void HandleDataBatchApplied(int64 FleetRevision, int32 UpdatedDroneCount);
};
