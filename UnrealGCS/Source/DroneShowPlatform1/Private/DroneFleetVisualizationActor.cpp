/**
 * @brief 实现 Fleet -> WGS84/Cesium -> UE World Position -> Niagara 数组的数据桥。
 *
 * 分类：Production。
 * Offline 只改变状态，仍保留粒子和 DroneOrder -> ParticleIndex；无效 GPS 使用 ZeroVector。
 */
#include "DroneFleetVisualizationActor.h"

#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "CesiumGeoreference.h"
#include "Components/SceneComponent.h"
#include "DroneLogicalId.h"
#include "EngineUtils.h"
#include "FleetManagerSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraSystem.h"

namespace
{
	const FName DroneCountParameter(TEXT("User.DroneCount"));
	const FName DronePositionsParameter(TEXT("User.DronePositions"));
	const FName DroneColorsParameter(TEXT("User.DroneColors"));
	const FName DroneDisplayIDsParameter(TEXT("User.DroneDisplayIDs"));
	const FName DroneSelectedMasksParameter(TEXT("User.DroneSelectedMasks"));
	const FName DroneLabelsVisibleParameter(TEXT("User.ShowDroneLabels"));
	const FName DroneMeshVisibleParameter(TEXT("User.ShowDroneMesh"));
	const FName LiveDroneHaloVisibleParameter(TEXT("User.ShowLiveDroneHalo"));
	const FName FlightMarkerSizeParameter(TEXT("User.FlightMarkerSize"));
	const FName RouteTargetCountParameter(TEXT("User.RouteTargetCount"));
	const FName RouteTargetPositionsParameter(TEXT("User.RouteTargetPositions"));
	const FName RouteTargetsVisibleParameter(TEXT("User.ShowRouteTargets"));

	constexpr float FlightMarkerTargetPixels = 6.0f;
	constexpr float FlightMarkerMinWorldSize = 24.0f;
	constexpr float FlightMarkerMaxWorldSize = 64.0f;
	constexpr float LivePositionUpdateIntervalSeconds = 1.0f / 60.0f;
	constexpr int64 LivePositionInterpolationDelayMs = 300;
	constexpr float FlightMarkerInterpolationSpeed = 8.0f;
	constexpr float FlightMarkerWriteThreshold = 0.25f;
	constexpr float DroneSphereWorldDiameterUU = 30.0f;
	constexpr float EnterMeshProjectedDiameterPixels = 14.0f;
	constexpr float ExitMeshProjectedDiameterPixels = 10.0f;

	int64 UtcNowMilliseconds()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}
}

ADroneFleetVisualizationActor::ADroneFleetVisualizationActor()
{
	// One bounded global update keeps the shared marker renderer screen-readable.
	// This remains independent of fleet size and never creates per-drone ticks.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = LivePositionUpdateIntervalSeconds;
	SetAutoDestroyWhenFinished(false);
	InitialLifeSpan = 0.0f;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	// 整个 Fleet 只由这一个 NiagaraComponent 渲染；禁止创建 per-drone Actor、
	// per-drone NiagaraComponent 或 per-drone Tick，以保持千机规模的固定对象开销。
	DroneFleetNiagara = CreateDefaultSubobject<UNiagaraComponent>(TEXT("DroneFleetNiagara"));
	DroneFleetNiagara->SetupAttachment(SceneRoot);
	DroneFleetNiagara->SetAutoActivate(false);
	DroneFleetNiagara->SetAutoDestroy(false);
	// Keep the moving drone markers above the coordinate-axis world overlay.
	// A spatial Z offset is insufficient because live drones can move above and
	// below the axis plane; the foreground depth group provides stable ordering.
	DroneFleetNiagara->SetDepthPriorityGroup(SDPG_Foreground);
	DroneFleetNiagara->SetTranslucentSortPriority(10);
}

void ADroneFleetVisualizationActor::BeginPlay()
{
	Super::BeginPlay();
	CacheCesiumGeoreference();

	if (!DroneFleetNiagara)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DroneFleetViz] Niagara component is unavailable; actor will remain in the level."));
		return;
	}

	if (!DroneFleetSystem)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DroneFleetViz] Niagara system is not assigned; actor will remain in the level."));
		return;
	}

	if (!GetGameInstance())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DroneFleetViz] GameInstance is unavailable; actor will remain in the level."));
		return;
	}

	DroneFleetNiagara->SetAsset(DroneFleetSystem);
	UpdatePrimaryDroneRendererVisibility();
	SetDroneLabelsVisible(false);
	ClearRouteTargets();
	BindFleetManager();
	if (!FleetManager.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DroneFleetViz] FleetManager is unavailable; actor will remain in the level."));
		return;
	}

	RefreshFromFleet();
}

void ADroneFleetVisualizationActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FlushDeferredNiagaraArrayRebind();
	UpdateLiveInterpolatedPositions(UtcNowMilliseconds(), true);
	UpdateAdaptiveFlightMarkerSize(DeltaSeconds);
}

void ADroneFleetVisualizationActor::UpdateAdaptiveFlightMarkerSize(const float DeltaSeconds)
{
	bLastFlightMarkerCalculationValid = false;
	bLastSphereProjectedDiameterValid = false;
	LastSphereProjectedDiameterPixels = 0.0f;
	if (!DroneFleetNiagara || !DroneFleetNiagara->GetAsset())
	{
		return;
	}

	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (!PlayerController || !CameraManager)
	{
		return;
	}

	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
	LastFlightMarkerViewportWidth = ViewportWidth;
	LastFlightMarkerViewportHeight = ViewportHeight;
	if (ViewportWidth <= 0 || ViewportHeight <= 0)
	{
		return;
	}

	const FMinimalViewInfo& CameraView = CameraManager->GetCameraCacheView();
	bLastFlightMarkerCalculationOrthographic =
		CameraView.ProjectionMode == ECameraProjectionMode::Orthographic;
	LastFlightMarkerCameraDistance = 0.0;
	LastFlightMarkerFOV = CameraView.FOV;
	LastFlightMarkerOrthoWidth = CameraView.OrthoWidth;
	float DesiredWorldSize = 0.0f;
	if (CameraView.ProjectionMode == ECameraProjectionMode::Orthographic)
	{
		if (!FMath::IsFinite(CameraView.OrthoWidth)
			|| CameraView.OrthoWidth <= UE_DOUBLE_SMALL_NUMBER)
		{
			return;
		}

		// UE OrthoWidth is the horizontal world-space span represented by the
		// viewport width. This is the exact inverse of the existing Marker sizing.
		const double SphereProjectedDiameterPixels =
			static_cast<double>(DroneSphereWorldDiameterUU)
			* static_cast<double>(ViewportWidth)
			/ static_cast<double>(CameraView.OrthoWidth);
		if (!UpdatePrimaryDroneDisplayModeForProjectedDiameter(
			SphereProjectedDiameterPixels))
		{
			return;
		}

		DesiredWorldSize = FlightMarkerTargetPixels
			* CameraView.OrthoWidth
			/ static_cast<float>(ViewportWidth);
	}
	else
	{
		if (!bCachedFleetVisualCenterValid)
		{
			return;
		}

		const double CameraDistance = FVector::Distance(
			CameraView.Location,
			CachedFleetVisualCenter);
		LastFlightMarkerCameraDistance = CameraDistance;
		if (!FMath::IsFinite(CameraDistance) || CameraDistance <= 0.0)
		{
			return;
		}

		const double AspectRatio = static_cast<double>(ViewportWidth)
			/ static_cast<double>(ViewportHeight);
		const double HorizontalHalfFovRadians = FMath::DegreesToRadians(
			static_cast<double>(CameraView.FOV) * 0.5);
		const double TanVerticalHalfFov = FMath::Tan(HorizontalHalfFovRadians)
			/ AspectRatio;
		if (!FMath::IsFinite(TanVerticalHalfFov) || TanVerticalHalfFov <= 0.0)
		{
			return;
		}

		const double VisibleWorldHeight = 2.0 * CameraDistance * TanVerticalHalfFov;
		if (!FMath::IsFinite(VisibleWorldHeight) || VisibleWorldHeight <= UE_DOUBLE_SMALL_NUMBER)
		{
			return;
		}

		const double SphereProjectedDiameterPixels =
			static_cast<double>(DroneSphereWorldDiameterUU)
			* static_cast<double>(ViewportHeight)
			/ VisibleWorldHeight;
		if (!UpdatePrimaryDroneDisplayModeForProjectedDiameter(
			SphereProjectedDiameterPixels))
		{
			return;
		}

		DesiredWorldSize = static_cast<float>(
			FlightMarkerTargetPixels
			* VisibleWorldHeight
			/ static_cast<double>(ViewportHeight));
	}

	if (!FMath::IsFinite(DesiredWorldSize))
	{
		return;
	}
	LastRawFlightMarkerSize = DesiredWorldSize;
	DesiredWorldSize = FMath::Clamp(
		DesiredWorldSize,
		FlightMarkerMinWorldSize,
		FlightMarkerMaxWorldSize);
	LastClampedFlightMarkerSize = DesiredWorldSize;
	bLastFlightMarkerCalculationValid = true;

	if (!bAdaptiveFlightMarkerSizeInitialized)
	{
		CurrentAdaptiveFlightMarkerSize = DesiredWorldSize;
		bAdaptiveFlightMarkerSizeInitialized = true;
	}
	else
	{
		CurrentAdaptiveFlightMarkerSize = FMath::FInterpTo(
			CurrentAdaptiveFlightMarkerSize,
			DesiredWorldSize,
			DeltaSeconds,
			FlightMarkerInterpolationSpeed);
	}

	if (LastSubmittedFlightMarkerSize >= 0.0f
		&& FMath::Abs(CurrentAdaptiveFlightMarkerSize - LastSubmittedFlightMarkerSize)
			< FlightMarkerWriteThreshold)
	{
		return;
	}

	DroneFleetNiagara->SetVariableVec2(
		FlightMarkerSizeParameter,
		FVector2D(CurrentAdaptiveFlightMarkerSize, CurrentAdaptiveFlightMarkerSize));
	LastSubmittedFlightMarkerSize = CurrentAdaptiveFlightMarkerSize;
}

void ADroneFleetVisualizationActor::LogFlightMarkerStatus() const
{
	const TCHAR* const ModeName = VisualizationMode == EGCSVisualizationMode::RoutePreview
		? TEXT("RoutePreview")
		: TEXT("Live");
	const TCHAR* const ProjectionName = bLastFlightMarkerCalculationValid
		? (bLastFlightMarkerCalculationOrthographic ? TEXT("Orthographic") : TEXT("Perspective"))
		: TEXT("Unavailable");
	const TCHAR* const DisplayModeName =
		DronePrimaryDisplayMode == EDronePrimaryDisplayMode::Mesh
			? TEXT("Mesh")
			: TEXT("Marker");

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[FlightMarker] Status Mode=%s DroneCount=%d RouteTargetCount=%d DisplayMode=%s ShowDroneMesh=%s ShowLiveDroneHalo=%s ShowRouteTargets=%s FleetVisualCenter=(%.3f,%.3f,%.3f) FleetVisualCenterValid=%s CameraDistanceReference=FleetCenter SphereWorldDiameterUU=%.3f SphereProjectedDiameterPx=%.3f SphereProjectionValid=%s EnterMeshThresholdPx=%.3f ExitMeshThresholdPx=%.3f MarkerSize=(%.3f,%.3f) Projection=%s Viewport=%dx%d CameraDistance=%.3f FOV=%.3f OrthoWidth=%.3f RawSize=%.3f ClampedSize=%.3f LastNiagaraWrite=%.3f CalculationValid=%s"),
		ModeName,
		DronePositions.Num(),
		RouteTargetPositions.Num(),
		DisplayModeName,
		bDroneMeshVisible ? TEXT("YES") : TEXT("NO"),
		bLiveDroneHaloVisible ? TEXT("YES") : TEXT("NO"),
		bRouteTargetsVisible ? TEXT("YES") : TEXT("NO"),
		CachedFleetVisualCenter.X,
		CachedFleetVisualCenter.Y,
		CachedFleetVisualCenter.Z,
		bCachedFleetVisualCenterValid ? TEXT("YES") : TEXT("NO"),
		DroneSphereWorldDiameterUU,
		LastSphereProjectedDiameterPixels,
		bLastSphereProjectedDiameterValid ? TEXT("YES") : TEXT("NO"),
		EnterMeshProjectedDiameterPixels,
		ExitMeshProjectedDiameterPixels,
		CurrentAdaptiveFlightMarkerSize,
		CurrentAdaptiveFlightMarkerSize,
		ProjectionName,
		LastFlightMarkerViewportWidth,
		LastFlightMarkerViewportHeight,
		LastFlightMarkerCameraDistance,
		LastFlightMarkerFOV,
		LastFlightMarkerOrthoWidth,
		LastRawFlightMarkerSize,
		LastClampedFlightMarkerSize,
		LastSubmittedFlightMarkerSize,
		bLastFlightMarkerCalculationValid ? TEXT("YES") : TEXT("NO"));
}

void ADroneFleetVisualizationActor::SetDroneLabelsVisible(const bool bVisible)
{
	if (DroneFleetNiagara)
	{
		DroneFleetNiagara->SetVariableBool(DroneLabelsVisibleParameter, bVisible);
	}
}

void ADroneFleetVisualizationActor::SetPrimaryDroneDisplayMode(
	const EDronePrimaryDisplayMode NewDisplayMode)
{
	if (bDroneRendererVisibilityInitialized
		&& DronePrimaryDisplayMode == NewDisplayMode)
	{
		return;
	}

	DronePrimaryDisplayMode = NewDisplayMode;
	UpdatePrimaryDroneRendererVisibility();
}

bool ADroneFleetVisualizationActor::UpdatePrimaryDroneDisplayModeForProjectedDiameter(
	const double ProjectedDiameterPixels)
{
	if (!FMath::IsFinite(ProjectedDiameterPixels) || ProjectedDiameterPixels < 0.0)
	{
		return false;
	}

	LastSphereProjectedDiameterPixels = static_cast<float>(ProjectedDiameterPixels);
	bLastSphereProjectedDiameterValid = true;
	if (DronePrimaryDisplayMode == EDronePrimaryDisplayMode::Marker
		&& ProjectedDiameterPixels > EnterMeshProjectedDiameterPixels)
	{
		SetPrimaryDroneDisplayMode(EDronePrimaryDisplayMode::Mesh);
	}
	else if (DronePrimaryDisplayMode == EDronePrimaryDisplayMode::Mesh
		&& ProjectedDiameterPixels < ExitMeshProjectedDiameterPixels)
	{
		SetPrimaryDroneDisplayMode(EDronePrimaryDisplayMode::Marker);
	}
	return true;
}

void ADroneFleetVisualizationActor::UpdatePrimaryDroneRendererVisibility()
{
	if (!DroneFleetNiagara)
	{
		return;
	}

	const bool bShouldShowMesh =
		DronePrimaryDisplayMode == EDronePrimaryDisplayMode::Mesh;
	const bool bShouldShowMarker = !bShouldShowMesh;
	if (bDroneRendererVisibilityInitialized
		&& bDroneMeshVisible == bShouldShowMesh
		&& bLiveDroneHaloVisible == bShouldShowMarker)
	{
		return;
	}

	// Disable the old renderer before enabling the new one. Both writes happen
	// on the game thread before Niagara renders the next frame; no cross-fade.
	if (bShouldShowMesh)
	{
		DroneFleetNiagara->SetVariableBool(LiveDroneHaloVisibleParameter, false);
		DroneFleetNiagara->SetVariableBool(DroneMeshVisibleParameter, true);
	}
	else
	{
		DroneFleetNiagara->SetVariableBool(DroneMeshVisibleParameter, false);
		DroneFleetNiagara->SetVariableBool(LiveDroneHaloVisibleParameter, true);
	}

	bDroneMeshVisible = bShouldShowMesh;
	bLiveDroneHaloVisible = bShouldShowMarker;
	bDroneRendererVisibilityInitialized = true;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[DroneViz] Primary display=%s ShowDroneMesh=%s ShowLiveDroneHalo=%s VisualizationMode=%s"),
		bShouldShowMesh ? TEXT("Mesh") : TEXT("Marker"),
		bDroneMeshVisible ? TEXT("YES") : TEXT("NO"),
		bLiveDroneHaloVisible ? TEXT("YES") : TEXT("NO"),
		VisualizationMode == EGCSVisualizationMode::Live ? TEXT("Live") : TEXT("RoutePreview"));
}

void ADroneFleetVisualizationActor::SetRouteTargetPositions(
	const TArray<FVector>& Positions)
{
	RouteTargetPositions = Positions;
	PushRouteTargetsToNiagara();
}

void ADroneFleetVisualizationActor::SetRouteTargetsVisible(const bool bVisible)
{
	bRouteTargetsVisible = bVisible
		&& VisualizationMode == EGCSVisualizationMode::Live
		&& !RouteTargetPositions.IsEmpty();
	if (DroneFleetNiagara)
	{
		DroneFleetNiagara->SetVariableBool(
			RouteTargetsVisibleParameter,
			bRouteTargetsVisible);
	}
}

void ADroneFleetVisualizationActor::SetRouteTargets(
	const TArray<FVector>& Positions,
	const bool bVisible)
{
	RouteTargetPositions = Positions;
	bRouteTargetsVisible = bVisible
		&& VisualizationMode == EGCSVisualizationMode::Live
		&& !RouteTargetPositions.IsEmpty();

	// Both emitters live in the same Niagara system. The combined push submits
	// Live and route-target state before a dynamic count change restarts it. The
	// target count always comes from Positions.Num(); it is never fixed.
	PushRouteTargetsToNiagara();
}

void ADroneFleetVisualizationActor::ClearRouteTargets()
{
	RouteTargetPositions.Reset();
	bRouteTargetsVisible = false;
	PushRouteTargetsToNiagara();
}

void ADroneFleetVisualizationActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindFleetManager();
	Super::EndPlay(EndPlayReason);
}

void ADroneFleetVisualizationActor::BindFleetManager()
{
	UnbindFleetManager();

	UGameInstance* GameInstance = GetGameInstance();
	UFleetManagerSubsystem* Fleet = GameInstance
		? GameInstance->GetSubsystem<UFleetManagerSubsystem>()
		: nullptr;
	if (!Fleet)
	{
		return;
	}

	FleetManager = Fleet;
	Fleet->OnFleetChanged.AddUniqueDynamic(this, &ADroneFleetVisualizationActor::HandleFleetChanged);
	Fleet->OnDataBatchApplied.AddUniqueDynamic(this, &ADroneFleetVisualizationActor::HandleDataBatchApplied);
}

void ADroneFleetVisualizationActor::UnbindFleetManager()
{
	if (UFleetManagerSubsystem* Fleet = FleetManager.Get())
	{
		Fleet->OnFleetChanged.RemoveDynamic(this, &ADroneFleetVisualizationActor::HandleFleetChanged);
		Fleet->OnDataBatchApplied.RemoveDynamic(this, &ADroneFleetVisualizationActor::HandleDataBatchApplied);
	}
	FleetManager.Reset();
}

void ADroneFleetVisualizationActor::SetCommandSelectedDroneIDs(
	const TArray<FString>& DroneIDs)
{
	TSet<FString> NewSelection;
	NewSelection.Reserve(DroneIDs.Num());
	for (const FString& DroneID : DroneIDs)
	{
		if (!DroneID.IsEmpty())
		{
			NewSelection.Add(DroneID);
		}
	}

	bool bSelectionChanged = NewSelection.Num() != CommandSelectedDroneIDs.Num();
	if (!bSelectionChanged)
	{
		for (const FString& DroneID : NewSelection)
		{
			if (!CommandSelectedDroneIDs.Contains(DroneID))
			{
				bSelectionChanged = true;
				break;
			}
		}
	}
	if (!bSelectionChanged)
	{
		return;
	}

	CommandSelectedDroneIDs = MoveTemp(NewSelection);
	RebuildDroneLabelAttributes();
	PushDroneLabelAttributesToNiagara();
}

void ADroneFleetVisualizationActor::SetDroneColorsForIDs(
	const TArray<FString>& DroneIDs,
	const FLinearColor& Color)
{
	if (DroneIDs.IsEmpty() || ParticleDroneIDs.IsEmpty())
	{
		return;
	}

	TSet<FString> TargetDroneIDs;
	TargetDroneIDs.Reserve(DroneIDs.Num());
	for (const FString& DroneID : DroneIDs)
	{
		if (!DroneID.IsEmpty())
		{
			TargetDroneIDs.Add(DroneID);
		}
	}
	if (TargetDroneIDs.IsEmpty())
	{
		return;
	}

	if (DroneColors.Num() != ParticleDroneIDs.Num())
	{
		const int32 PreviousColorCount = DroneColors.Num();
		DroneColors.SetNum(ParticleDroneIDs.Num());
		for (int32 Index = PreviousColorCount; Index < DroneColors.Num(); ++Index)
		{
			DroneColors[Index] = FLinearColor::Black;
		}
		bDroneColorsDirty = true;
	}

	bool bAppliedColor = false;
	for (int32 StableParticleIndex = 0;
		StableParticleIndex < ParticleDroneIDs.Num();
		++StableParticleIndex)
	{
		if (TargetDroneIDs.Contains(ParticleDroneIDs[StableParticleIndex]))
		{
			DroneColors[StableParticleIndex] = Color;
			bAppliedColor = true;
		}
	}

	if (bAppliedColor)
	{
		bDroneColorsDirty = true;
		PushDroneColorsToNiagara();
	}
}

void ADroneFleetVisualizationActor::SetConfirmedRouteSlotDisplayOverrides(
	const TMap<FString, int32>& OverridesByDroneID)
{
	TMap<FString, int32> Validated;
	Validated.Reserve(OverridesByDroneID.Num());
	for (const TPair<FString, int32>& Override : OverridesByDroneID)
	{
		if (!Override.Key.TrimStartAndEnd().IsEmpty() && Override.Value >= 0)
		{
			Validated.Add(Override.Key, Override.Value);
		}
	}
	if (ConfirmedRouteSlotDisplayOverrides.OrderIndependentCompareEqual(Validated))
	{
		return;
	}
	ConfirmedRouteSlotDisplayOverrides = MoveTemp(Validated);
	RebuildDroneLabelAttributes();
	PushDroneLabelAttributesToNiagara();
}

void ADroneFleetVisualizationActor::ClearConfirmedRouteSlotDisplayOverrides()
{
	if (ConfirmedRouteSlotDisplayOverrides.IsEmpty())
	{
		return;
	}
	ConfirmedRouteSlotDisplayOverrides.Reset();
	RebuildDroneLabelAttributes();
	PushDroneLabelAttributesToNiagara();
}

void ADroneFleetVisualizationActor::SetFixedPileDroneID(const FString& DroneID)
{
	if (FixedPileDroneID.Equals(DroneID, ESearchCase::CaseSensitive))
	{
		return;
	}
	FixedPileDroneID = DroneID;
	RebuildDroneLabelAttributes();
	PushDroneLabelAttributesToNiagara();
}

void ADroneFleetVisualizationActor::ClearFixedPileDroneID()
{
	if (FixedPileDroneID.IsEmpty())
	{
		return;
	}
	FixedPileDroneID.Reset();
	RebuildDroneLabelAttributes();
	PushDroneLabelAttributesToNiagara();
}

bool ADroneFleetVisualizationActor::TryResolveDroneIDFromDisplayID(
	const int32 DisplayID,
	FString& OutDroneID) const
{
	OutDroneID.Reset();
	if (DisplayID < 0)
	{
		return false;
	}

	const int32 Count = FMath::Min3(
		ParticleDroneIDs.Num(),
		DroneDisplayIDs.Num(),
		DroneSelectedMasks.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		// A negative selection mask is the existing hidden-label sentinel.
		if (DroneSelectedMasks[Index] < 0.0f
			|| FMath::RoundToInt(DroneDisplayIDs[Index]) != DisplayID)
		{
			continue;
		}
		if (!OutDroneID.IsEmpty())
		{
			// A displayed number must identify exactly one particle.
			OutDroneID.Reset();
			return false;
		}
		OutDroneID = ParticleDroneIDs[Index];
	}
	return !OutDroneID.IsEmpty();
}

bool ADroneFleetVisualizationActor::TryGetLiveDisplayIDSnapshot(
	TMap<FString, int32>& OutDisplayIDsByDroneID,
	FString& OutError) const
{
	OutDisplayIDsByDroneID.Reset();
	OutError.Reset();
	if (VisualizationMode != EGCSVisualizationMode::Live)
	{
		OutError = TEXT("当前正在航线预览");
		return false;
	}
	const int32 Count = ParticleDroneIDs.Num();
	if (Count <= 0
		|| DroneDisplayIDs.Num() != Count
		|| DroneSelectedMasks.Num() != Count
		|| DronePositions.Num() != Count)
	{
		OutError = TEXT("无人机显示快照尚未准备完成");
		return false;
	}

	TSet<int32> SeenDisplayIDs;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FString& DroneID = ParticleDroneIDs[Index];
		const float DisplayValue = DroneDisplayIDs[Index];
		if (DroneID.IsEmpty()
			|| DroneSelectedMasks[Index] < 0.0f
			|| !FMath::IsFinite(DisplayValue))
		{
			OutError = TEXT("存在隐藏或无效的无人机编号");
			OutDisplayIDsByDroneID.Reset();
			return false;
		}
		const int32 DisplayID = FMath::RoundToInt(DisplayValue);
		if (DisplayID < 0
			|| !FMath::IsNearlyEqual(DisplayValue, static_cast<float>(DisplayID))
			|| SeenDisplayIDs.Contains(DisplayID)
			|| OutDisplayIDsByDroneID.Contains(DroneID))
		{
			OutError = TEXT("存在重复或无效的无人机编号");
			OutDisplayIDsByDroneID.Reset();
			return false;
		}
		SeenDisplayIDs.Add(DisplayID);
		OutDisplayIDsByDroneID.Add(DroneID, DisplayID);
	}
	return true;
}

void ADroneFleetVisualizationActor::SetNpzNumberingAnomalyDroneIDs(
	const TArray<FString>& DroneIDs)
{
	TSet<FString> Validated;
	Validated.Reserve(DroneIDs.Num());
	for (const FString& DroneID : DroneIDs)
	{
		if (!DroneID.TrimStartAndEnd().IsEmpty())
		{
			Validated.Add(DroneID);
		}
	}
	if (NpzNumberingAnomalyDroneIDs.Num() == Validated.Num()
		&& NpzNumberingAnomalyDroneIDs.Includes(Validated))
	{
		return;
	}
	NpzNumberingAnomalyDroneIDs = MoveTemp(Validated);
	RebuildDroneLabelAttributes();
	bDroneColorsDirty = true;
	PushDroneLabelAttributesToNiagara();
	PushDroneColorsToNiagara();
}

void ADroneFleetVisualizationActor::ClearNpzNumberingAnomalyDroneIDs()
{
	if (NpzNumberingAnomalyDroneIDs.IsEmpty())
	{
		return;
	}
	NpzNumberingAnomalyDroneIDs.Reset();
	RebuildDroneLabelAttributes();
	bDroneColorsDirty = true;
	PushDroneLabelAttributesToNiagara();
	PushDroneColorsToNiagara();
}

void ADroneFleetVisualizationActor::RefreshFromFleet()
{
	// Telemetry continues to update FleetManager during preview. Only the render
	// bridge is source-gated, so live authority is never overwritten or paused.
	if (VisualizationMode == EGCSVisualizationMode::RoutePreview)
	{
		return;
	}

	CacheCesiumGeoreference();

	UFleetManagerSubsystem* Fleet = FleetManager.Get();
	if (!Fleet)
	{
		UGameInstance* GameInstance = GetGameInstance();
		Fleet = GameInstance ? GameInstance->GetSubsystem<UFleetManagerSubsystem>() : nullptr;
		FleetManager = Fleet;
	}

	DronePositions.Reset();
	bCachedFleetVisualCenterValid = false;
	CachedFleetVisualCenter = FVector::ZeroVector;
	TMap<FString, FLinearColor> ExistingColorsByDroneID;
	ExistingColorsByDroneID.Reserve(FMath::Min(ParticleDroneIDs.Num(), DroneColors.Num()));
	for (int32 Index = 0;
		Index < ParticleDroneIDs.Num() && Index < DroneColors.Num();
		++Index)
	{
		ExistingColorsByDroneID.Add(ParticleDroneIDs[Index], DroneColors[Index]);
	}
	TArray<FDroneData> OrderedDrones;
	TArray<FString> NewParticleDroneIDs;
	TArray<FLinearColor> NewDroneColors;
	TSet<FString> ActiveLiveDroneIDs;
	if (Fleet)
	{
		// GetDronePage 保留 Fleet DroneOrder，因此 ParticleIndex -> DroneID 映射稳定。
		// 不得按 TMap、GPS、Online 或选择状态重排；Offline 也必须保留原索引。
		OrderedDrones = Fleet->GetDronePage(0, Fleet->GetDroneCount());
		NewParticleDroneIDs.Reserve(OrderedDrones.Num());
		NewDroneColors.Reserve(OrderedDrones.Num());
		ActiveLiveDroneIDs.Reserve(OrderedDrones.Num());
		DronePositions.SetNumZeroed(OrderedDrones.Num());

		const FFleetGeoOrigin FleetGeoOrigin = Fleet->GetFleetGeoOrigin();
		UpdateOriginWorldPosition(FleetGeoOrigin);
		for (int32 Index = 0; Index < OrderedDrones.Num(); ++Index)
		{
			const FDroneData& Drone = OrderedDrones[Index];
			NewParticleDroneIDs.Add(Drone.DroneID);
			ActiveLiveDroneIDs.Add(Drone.DroneID);
			const FLinearColor* ExistingColor = ExistingColorsByDroneID.Find(Drone.DroneID);
			NewDroneColors.Add(
				Drone.bLEDColorValid
					? Drone.LEDColor
					: (ExistingColor ? *ExistingColor : Drone.LEDColor));

			// FleetGeoOrigin 尚未建立时，任何后续无人机都不能抢占逻辑原点。
			// 单机 GPS 无效时必须回到 ZeroVector，禁止保留历史坐标或使用索引占位。
			if (FleetGeoOrigin.bOriginInitialized
				&& bOriginWorldPositionInitialized
				&& DroneGPS::IsValid(Drone))
			{
				const FVector SampleWorldPosition = ConvertDroneGPSPosition(Drone, FleetGeoOrigin);
				CaptureLivePositionSample(
					Drone.DroneID,
					SampleWorldPosition,
					Drone.GPSSampleTimestampUtcMs);
				if (const FLivePositionInterpolationState* State = LivePositionStates.Find(Drone.DroneID))
				{
					DronePositions[Index] = State->RenderPosition;
				}
			}
			else
			{
				LivePositionStates.Remove(Drone.DroneID);
			}
		}

		for (auto StateIterator = LivePositionStates.CreateIterator(); StateIterator; ++StateIterator)
		{
			if (!ActiveLiveDroneIDs.Contains(StateIterator.Key()))
			{
				StateIterator.RemoveCurrent();
			}
		}

		// Offline 只改变状态，不删除粒子，也不改变 DroneOrder -> ParticleIndex 映射。
	}
	else
	{
		LivePositionStates.Reset();
	}

	const bool bParticleLayoutChanged = ParticleDroneIDs != NewParticleDroneIDs
		|| DroneColors.Num() != NewDroneColors.Num()
		|| DroneDisplayIDs.Num() != NewParticleDroneIDs.Num()
		|| DroneSelectedMasks.Num() != NewParticleDroneIDs.Num();
	const bool bColorsChanged = DroneColors != NewDroneColors;
	if (bParticleLayoutChanged)
	{
		ParticleDroneIDs = MoveTemp(NewParticleDroneIDs);
		RebuildDroneLabelAttributes();
	}
	if (bColorsChanged)
	{
		DroneColors = MoveTemp(NewDroneColors);
		bDroneColorsDirty = true;
	}

	UpdateLiveInterpolatedPositions(UtcNowMilliseconds(), false);
	FBox FleetVisualBounds(ForceInit);
	for (int32 Index = 0; Index < OrderedDrones.Num() && Index < DronePositions.Num(); ++Index)
	{
		if (DroneGPS::IsValid(OrderedDrones[Index]))
		{
			FleetVisualBounds += DronePositions[Index];
		}
	}
	if (FleetVisualBounds.IsValid)
	{
		CachedFleetVisualCenter = FleetVisualBounds.GetCenter();
		bCachedFleetVisualCenterValid = true;
	}

	PushArraysToNiagara();

	// 仅为定位GPS有效后粒子不可见的问题记录一次运行快照，不参与任何位置计算。
	static TSet<uint32> DiagnosedActorIds;
	const uint32 ActorId = GetUniqueID();
	ACesiumGeoreference* Georeference = CesiumGeoreference.Get();
	const bool bFirstThreeGPSValid = OrderedDrones.Num() >= 3
		&& DroneGPS::IsValid(OrderedDrones[0])
		&& DroneGPS::IsValid(OrderedDrones[1])
		&& DroneGPS::IsValid(OrderedDrones[2]);
	if (!DiagnosedActorIds.Contains(ActorId)
		&& Georeference
		&& bFirstThreeGPSValid
		&& DronePositions.Num() >= 3)
	{
		DiagnosedActorIds.Add(ActorId);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[DroneFleetViz Debug] GeoreferenceOrigin=(Lon=%.8f Lat=%.8f Height=%.3f) ActorTransform=%s"),
			Georeference->GetOriginLongitude(),
			Georeference->GetOriginLatitude(),
			Georeference->GetOriginHeight(),
			*Georeference->GetActorTransform().ToHumanReadableString());

		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FDroneData& Drone = OrderedDrones[Index];
			const FVector& WorldPosition = DronePositions[Index];
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[DroneFleetViz Debug] %s GPS=(%.8f,%.8f) GPSValid=%s Fix=%d RelativeAltitude=%.3f AltitudeAMSL=%.3f AltitudeValid=%s World=(%.3f,%.3f,%.3f)"),
				*Drone.DroneID,
				Drone.GPSLat,
				Drone.GPSLon,
				Drone.bGPSValid ? TEXT("true") : TEXT("false"),
				Drone.GPSFixType,
				Drone.Altitude,
				Drone.AltitudeAMSL,
				Drone.bAltitudeValid ? TEXT("true") : TEXT("false"),
				WorldPosition.X,
				WorldPosition.Y,
				WorldPosition.Z);
		}

		if (APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			const FVector CameraWorldPosition = CameraManager->GetCameraLocation();
			const double DistanceCentimeters = FVector::Distance(
				CameraWorldPosition,
				DronePositions[0]);
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[DroneFleetViz Debug] CameraWorld=(%.3f,%.3f,%.3f) CameraToFirstDrone=%.3fcm (%.3fkm)"),
				CameraWorldPosition.X,
				CameraWorldPosition.Y,
				CameraWorldPosition.Z,
				DistanceCentimeters,
				DistanceCentimeters / 100000.0);
		}

		UE_LOG(
			LogTemp,
			Display,
			TEXT("[DroneFleetViz Debug] Fleet=%d DronePositions=%d User.DroneCount=%d PositionArrayPushed=true NiagaraActive=%s"),
			Fleet ? Fleet->GetDroneCount() : 0,
			DronePositions.Num(),
			DronePositions.Num(),
			DroneFleetNiagara && DroneFleetNiagara->IsActive() ? TEXT("true") : TEXT("false"));
	}

	const int32 FleetCount = Fleet ? Fleet->GetDroneCount() : 0;
	const int32 ParticleCount = DronePositions.Num();
	if (LastLoggedFleetCount != FleetCount || LastLoggedParticleCount != ParticleCount)
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("[DroneFleetViz] Fleet=%d Particles=%d"),
			FleetCount,
			ParticleCount);
		LastLoggedFleetCount = FleetCount;
		LastLoggedParticleCount = ParticleCount;
	}

	if (FleetCount != ParticleCount)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[DroneFleetViz] Fleet/particle count mismatch: Fleet=%d Particles=%d"),
			FleetCount,
			ParticleCount);
	}
}

void ADroneFleetVisualizationActor::CacheCesiumGeoreference()
{
	if (bCesiumLookupAttempted)
	{
		return;
	}

	bCesiumLookupAttempted = true;
	UWorld* World = GetWorld();
	if (World)
	{
		for (TActorIterator<ACesiumGeoreference> It(World); It; ++It)
		{
			CesiumGeoreference = *It;
			break;
		}
	}

	if (CesiumGeoreference.IsValid())
	{
		if (!bCesiumReadyLogged)
		{
			UE_LOG(LogTemp, Display, TEXT("[DroneFleetViz] CesiumGeoreference ready"));
			bCesiumReadyLogged = true;
		}
	}
	else if (!bCesiumMissingLogged)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DroneFleetViz] CesiumGeoreference not found"));
		bCesiumMissingLogged = true;
	}
}

void ADroneFleetVisualizationActor::CaptureLivePositionSample(
	const FString& DroneID,
	const FVector& WorldPosition,
	const int64 TimestampUtcMs)
{
	FLivePositionInterpolationState& State = LivePositionStates.FindOrAdd(DroneID);
	if (TimestampUtcMs <= 0)
	{
		// Legacy/mock inputs have no sample time. Preserve their established
		// immediate-position behavior instead of fabricating timing information.
		State.bInitialized = true;
		State.bHasPrevious = false;
		State.Latest.TimestampUtcMs = 0;
		State.Latest.WorldPosition = WorldPosition;
		State.RenderPosition = WorldPosition;
		return;
	}

	if (!State.bInitialized)
	{
		State.bInitialized = true;
		State.bHasPrevious = false;
		State.Latest.TimestampUtcMs = TimestampUtcMs;
		State.Latest.WorldPosition = WorldPosition;
		State.RenderPosition = WorldPosition;
		return;
	}

	if (TimestampUtcMs < State.Latest.TimestampUtcMs)
	{
		// UDP snapshots can arrive out of order. Never rewind the live target.
		return;
	}
	if (TimestampUtcMs == State.Latest.TimestampUtcMs)
	{
		State.Latest.WorldPosition = WorldPosition;
		if (!State.bHasPrevious)
		{
			State.RenderPosition = WorldPosition;
		}
		return;
	}

	State.Previous = State.Latest;
	State.Latest.TimestampUtcMs = TimestampUtcMs;
	State.Latest.WorldPosition = WorldPosition;
	State.bHasPrevious = true;
}

bool ADroneFleetVisualizationActor::UpdateLiveInterpolatedPositions(
	const int64 NowUtcMs,
	const bool bPushToNiagara)
{
	if (VisualizationMode != EGCSVisualizationMode::Live)
	{
		return false;
	}

	const int64 RenderTimestampUtcMs = NowUtcMs - LivePositionInterpolationDelayMs;
	bool bAnyPositionChanged = false;
	for (int32 Index = 0; Index < ParticleDroneIDs.Num() && Index < DronePositions.Num(); ++Index)
	{
		FLivePositionInterpolationState* State = LivePositionStates.Find(ParticleDroneIDs[Index]);
		if (!State || !State->bInitialized)
		{
			continue;
		}

		FVector NewRenderPosition = State->Latest.WorldPosition;
		if (State->bHasPrevious
			&& State->Previous.TimestampUtcMs > 0
			&& State->Latest.TimestampUtcMs > State->Previous.TimestampUtcMs)
		{
			const double Alpha = FMath::Clamp(
				static_cast<double>(RenderTimestampUtcMs - State->Previous.TimestampUtcMs)
					/ static_cast<double>(State->Latest.TimestampUtcMs - State->Previous.TimestampUtcMs),
				0.0,
				1.0);
			NewRenderPosition = FMath::Lerp(
				State->Previous.WorldPosition,
				State->Latest.WorldPosition,
				Alpha);
		}

		State->RenderPosition = NewRenderPosition;
		if (!DronePositions[Index].Equals(NewRenderPosition, 0.01))
		{
			DronePositions[Index] = NewRenderPosition;
			bAnyPositionChanged = true;
		}
	}

	if (bAnyPositionChanged && bPushToNiagara)
	{
		PushDronePositionsToNiagara();
	}
	return bAnyPositionChanged;
}

#if WITH_DEV_AUTOMATION_TESTS
void ADroneFleetVisualizationActor::AdvanceLiveInterpolationForTest(const int64 NowUtcMs)
{
	UpdateLiveInterpolatedPositions(NowUtcMs, false);
}
#endif

bool ADroneFleetVisualizationActor::TryGetGeographicTangentBasis(
	FVector& OutOriginWorld,
	FVector& OutEastWorld,
	FVector& OutNorthWorld,
	FVector& OutUpWorld) const
{
	OutOriginWorld = FVector::ZeroVector;
	OutEastWorld = FVector::ZeroVector;
	OutNorthWorld = FVector::ZeroVector;
	OutUpWorld = FVector::ZeroVector;

	ACesiumGeoreference* Georeference = CesiumGeoreference.Get();
	if (!Georeference)
	{
		return false;
	}

	FVector GeographicBasisOriginWorld = FVector::ZeroVector;
	if (VisualizationMode == EGCSVisualizationMode::RoutePreview
		&& bRoutePreviewOriginWorldPositionInitialized)
	{
		OutOriginWorld = RoutePreviewOriginWorldPosition;
		GeographicBasisOriginWorld = RoutePreviewOriginWorldPosition;
	}
	else if (bOriginWorldPositionInitialized)
	{
		OutOriginWorld = OriginWorldPosition;
		GeographicBasisOriginWorld = GeographicOriginWorldPosition;
	}
	else
	{
		return false;
	}

	const FTransform GeoreferenceActorTransform = Georeference->GetActorTransform();
	const FVector OriginInGeoreference =
		GeoreferenceActorTransform.InverseTransformPosition(GeographicBasisOriginWorld);
	const FMatrix EastSouthUpToGeoreference =
		Georeference->ComputeEastSouthUpToUnrealTransformation(OriginInGeoreference);

	const auto TransformDirectionToWorld =
		[&EastSouthUpToGeoreference, &GeoreferenceActorTransform](const FVector& Direction)
		{
			return GeoreferenceActorTransform.TransformVector(
				EastSouthUpToGeoreference.TransformVector(Direction));
		};

	const FVector EastRaw = TransformDirectionToWorld(FVector::XAxisVector);
	const FVector NorthRaw = -TransformDirectionToWorld(FVector::YAxisVector);
	const FVector UpRaw = TransformDirectionToWorld(FVector::ZAxisVector);
	if (EastRaw.ContainsNaN() || NorthRaw.ContainsNaN() || UpRaw.ContainsNaN())
	{
		return false;
	}

	const FVector Up = UpRaw.GetSafeNormal();
	const FVector East = (EastRaw - Up * FVector::DotProduct(EastRaw, Up)).GetSafeNormal();
	const FVector NorthFromCesium =
		(NorthRaw
			- Up * FVector::DotProduct(NorthRaw, Up)
			- East * FVector::DotProduct(NorthRaw, East)).GetSafeNormal();
	const FVector OrthogonalNorth = FVector::CrossProduct(-Up, East).GetSafeNormal();
	if (Up.IsNearlyZero()
		|| East.IsNearlyZero()
		|| NorthFromCesium.IsNearlyZero()
		|| OrthogonalNorth.IsNearlyZero()
		|| FVector::DotProduct(NorthFromCesium, OrthogonalNorth) < 0.999f)
	{
		return false;
	}

	OutEastWorld = East;
	OutNorthWorld = OrthogonalNorth;
	OutUpWorld = Up;
	return true;
}

void ADroneFleetVisualizationActor::UpdateOriginWorldPosition(const FFleetGeoOrigin& FleetGeoOrigin)
{
	if (!FleetGeoOrigin.bOriginInitialized)
	{
		LivePositionStates.Reset();
		bOriginWorldPositionInitialized = false;
		OriginWorldPosition = FVector::ZeroVector;
		GeographicOriginWorldPosition = FVector::ZeroVector;
		CachedOriginDroneID.Reset();
		return;
	}

	ACesiumGeoreference* Georeference = CesiumGeoreference.Get();
	if (!Georeference)
	{
		LivePositionStates.Reset();
		bOriginWorldPositionInitialized = false;
		OriginWorldPosition = FVector::ZeroVector;
		GeographicOriginWorldPosition = FVector::ZeroVector;
		return;
	}

	const FVector DisplayAnchorWorld = GetActorLocation();
	const bool bSameLockedOrigin = bOriginWorldPositionInitialized
		&& CachedOriginDroneID == FleetGeoOrigin.OriginDroneID
		&& CachedOriginLatitude == FleetGeoOrigin.OriginLatitude
		&& CachedOriginLongitude == FleetGeoOrigin.OriginLongitude
		&& CachedOriginAltitudeAMSL == FleetGeoOrigin.OriginAltitudeAMSL
		&& bCachedOriginAltitudeValid == FleetGeoOrigin.bOriginAltitudeValid
		&& OriginWorldPosition.Equals(DisplayAnchorWorld);
	if (bSameLockedOrigin)
	{
		return;
	}

	// Position samples are expressed in the current display basis. A changed
	// fixed pile, geographic origin, or actor anchor invalidates the old basis.
	LivePositionStates.Reset();

	// FleetGeoOrigin 是机群逻辑原点；CesiumGeoreference 是地图坐标参考。
	// 此处只读取 Cesium 转换结果，绝不移动或重设整个 Cesium 世界的 Origin。
	const double HeightMeters = FleetGeoOrigin.bOriginAltitudeValid
		? static_cast<double>(FleetGeoOrigin.OriginAltitudeAMSL)
		: 0.0;
	const FVector GeoreferencePosition =
		Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(
			FleetGeoOrigin.OriginLongitude,
			FleetGeoOrigin.OriginLatitude,
			HeightMeters));
	GeographicOriginWorldPosition =
		Georeference->GetActorTransform().TransformPosition(GeoreferencePosition);
	if (GeographicOriginWorldPosition.ContainsNaN())
	{
		LivePositionStates.Reset();
		OriginWorldPosition = FVector::ZeroVector;
		GeographicOriginWorldPosition = FVector::ZeroVector;
		bOriginWorldPositionInitialized = false;
		return;
	}
	OriginWorldPosition = DisplayAnchorWorld;

	CachedOriginDroneID = FleetGeoOrigin.OriginDroneID;
	CachedOriginLatitude = FleetGeoOrigin.OriginLatitude;
	CachedOriginLongitude = FleetGeoOrigin.OriginLongitude;
	CachedOriginAltitudeAMSL = FleetGeoOrigin.OriginAltitudeAMSL;
	bCachedOriginAltitudeValid = FleetGeoOrigin.bOriginAltitudeValid;
	bOriginWorldPositionInitialized = true;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("[DroneFleetViz] GeoOrigin initialized Drone=%s Lat=%.8f Lon=%.8f"),
		*CachedOriginDroneID,
		CachedOriginLatitude,
		CachedOriginLongitude);
}

FVector ADroneFleetVisualizationActor::ConvertDroneGPSPosition(
	const FDroneData& Drone,
	const FFleetGeoOrigin& FleetGeoOrigin) const
{
	ACesiumGeoreference* Georeference = CesiumGeoreference.Get();
	// 只有有效 WGS84 GPS 才进入 CesiumGeoreference。无效 GPS 固定返回
	// ZeroVector，不使用随机/矩阵占位，也不沿用最后已知位置；AMSL=0 本身是合法高度。
	if (!Georeference || !FleetGeoOrigin.bOriginInitialized || !DroneGPS::IsValid(Drone))
	{
		return FVector::ZeroVector;
	}

	// MAVLink AMSL 与 Cesium 椭球高存在高程基准差异。本阶段用于位置可视化，
	// 后续可增加 Geoid/高程基准转换；禁止把 Relative Altitude 当作绝对高度。
	const double HeightMeters = Drone.bAltitudeValid
		? static_cast<double>(Drone.AltitudeAMSL)
		: (FleetGeoOrigin.bOriginAltitudeValid
			? static_cast<double>(FleetGeoOrigin.OriginAltitudeAMSL)
			: 0.0);
	const FVector GeoreferencePosition =
		Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(
			Drone.GPSLon,
			Drone.GPSLat,
			HeightMeters));
	const FVector GeographicWorldPosition =
		Georeference->GetActorTransform().TransformPosition(GeoreferencePosition);
	if (GeographicWorldPosition.ContainsNaN() || !bOriginWorldPositionInitialized)
	{
		return FVector::ZeroVector;
	}
	return OriginWorldPosition + (GeographicWorldPosition - GeographicOriginWorldPosition);
}

void ADroneFleetVisualizationActor::RebuildDroneLabelAttributes()
{
	const int32 ParticleCount = DronePositions.Num();
	DroneDisplayIDs.SetNumUninitialized(ParticleCount);
	DroneSelectedMasks.SetNumUninitialized(ParticleCount);

	std::uint64_t FixedPilePreviousDisplayID = 0;
	bool bHasFixedPilePreviousDisplayID = false;
	FString ZeroDisplayDroneID;
	for (const FString& DroneID : ParticleDroneIDs)
	{
		if (NpzNumberingAnomalyDroneIDs.Contains(DroneID))
		{
			continue;
		}
		std::uint64_t NumericDroneID = 0;
		FTCHARToUTF8 EncodedDroneID(*DroneID);
		if (!dronegcs::identity::TryParseDroneLogicalId(
			std::string_view(EncodedDroneID.Get(), EncodedDroneID.Length()),
			NumericDroneID))
		{
			continue;
		}
		// Fixed-pile swapping is intentionally based on the non-NPZ number. NPZ
		// RouteSlot overrides are applied later and always have higher priority.
		if (NumericDroneID == 0 && ZeroDisplayDroneID.IsEmpty())
		{
			ZeroDisplayDroneID = DroneID;
		}
		if (DroneID.Equals(FixedPileDroneID, ESearchCase::CaseSensitive))
		{
			FixedPilePreviousDisplayID = NumericDroneID;
			bHasFixedPilePreviousDisplayID = true;
		}
	}
	for (int32 Index = 0; Index < ParticleCount; ++Index)
	{
		// Full route preview particles are ordered by zero-based NPZ RouteSlot and
		// deliberately have no physical DroneID. Keep that preview-only identity
		// separate from the canonical Live DroneID rules below.
		if (VisualizationMode == EGCSVisualizationMode::RoutePreview)
		{
			DroneDisplayIDs[Index] = static_cast<float>(Index);
			DroneSelectedMasks[Index] = 0.0f;
			continue;
		}

		const bool bHasDroneID = ParticleDroneIDs.IsValidIndex(Index);
		const FString DroneID = bHasDroneID ? ParticleDroneIDs[Index] : FString();
		const bool bNumberingAnomaly = bHasDroneID
			&& NpzNumberingAnomalyDroneIDs.Contains(DroneID);
		const int32* ConfirmedRouteSlot = bHasDroneID
			? ConfirmedRouteSlotDisplayOverrides.Find(DroneID)
			: nullptr;
		const bool bIsFixedPile = bHasDroneID
			&& bHasFixedPilePreviousDisplayID
			&& DroneID.Equals(FixedPileDroneID, ESearchCase::CaseSensitive);
		const bool bIsZeroDisplayDrone = bHasDroneID
			&& bHasFixedPilePreviousDisplayID
			&& !ZeroDisplayDroneID.Equals(FixedPileDroneID, ESearchCase::CaseSensitive)
			&& DroneID.Equals(ZeroDisplayDroneID, ESearchCase::CaseSensitive);

		std::uint64_t NumericDroneID = 0;
		bool bHasCanonicalDroneID = false;
		if (bHasDroneID)
		{
			FTCHARToUTF8 EncodedDroneID(*DroneID);
			bHasCanonicalDroneID = dronegcs::identity::TryParseDroneLogicalId(
				std::string_view(EncodedDroneID.Get(), EncodedDroneID.Length()),
				NumericDroneID);
		}
		const bool bHideLabel = bNumberingAnomaly || !bHasCanonicalDroneID;
		const std::uint64_t DisplayID = ConfirmedRouteSlot
			? static_cast<std::uint64_t>(*ConfirmedRouteSlot)
			: (bIsFixedPile
				? 0
				: (bIsZeroDisplayDrone ? FixedPilePreviousDisplayID : NumericDroneID));
		DroneDisplayIDs[Index] = static_cast<float>(bHideLabel ? 0 : DisplayID);
		// -1 is a per-particle label-hidden sentinel consumed by M_DroneIdLabel.
		DroneSelectedMasks[Index] = bHideLabel
			? -1.0f
			: (VisualizationMode == EGCSVisualizationMode::Live
			&& CommandSelectedDroneIDs.Contains(DroneID)
			? 1.0f
			: 0.0f);
	}
	bDroneLabelAttributesDirty = true;
}

void ADroneFleetVisualizationActor::PushDroneLabelAttributesToNiagara()
{
	if (!bDroneLabelAttributesDirty || !DroneFleetNiagara || !DroneFleetNiagara->GetAsset())
	{
		return;
	}

	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayFloat(
		DroneFleetNiagara,
		DroneDisplayIDsParameter,
		DroneDisplayIDs);
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayFloat(
		DroneFleetNiagara,
		DroneSelectedMasksParameter,
		DroneSelectedMasks);
	bDroneLabelAttributesDirty = false;
}

void ADroneFleetVisualizationActor::PushDronePositionsToNiagara()
{
	if (!DroneFleetNiagara || !DroneFleetNiagara->GetAsset())
	{
		return;
	}
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayPosition(
		DroneFleetNiagara,
		DronePositionsParameter,
		DronePositions);
}

bool ADroneFleetVisualizationActor::PushDroneColorsToNiagara()
{
	if (!bDroneColorsDirty || !DroneFleetNiagara || !DroneFleetNiagara->GetAsset())
	{
		return false;
	}
	if (DroneColors.Num() != DronePositions.Num())
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[DroneColors] Niagara write rejected: Colors=%d Particles=%d"),
			DroneColors.Num(),
			DronePositions.Num());
		return false;
	}

	TArray<FLinearColor> EffectiveColors;
	const TArray<FLinearColor>* ColorsToPush = &DroneColors;
	if (!NpzNumberingAnomalyDroneIDs.IsEmpty())
	{
		EffectiveColors = DroneColors;
		for (int32 Index = 0; Index < EffectiveColors.Num() && Index < ParticleDroneIDs.Num(); ++Index)
		{
			if (NpzNumberingAnomalyDroneIDs.Contains(ParticleDroneIDs[Index]))
			{
				EffectiveColors[Index] = FLinearColor::Red;
			}
		}
		ColorsToPush = &EffectiveColors;
	}
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayColor(
		DroneFleetNiagara,
		DroneColorsParameter,
		*ColorsToPush);
	bDroneColorsDirty = false;
	return true;
}

void ADroneFleetVisualizationActor::PushArraysToNiagara()
{
	if (!DroneFleetNiagara || !DroneFleetNiagara->GetAsset())
	{
		return;
	}

	const int32 NewDroneCount = DronePositions.Num();
	const bool bCountChanged = LastPushedDroneCount != NewDroneCount;
	DroneFleetNiagara->SetVariableInt(DroneCountParameter, NewDroneCount);
	PushDronePositionsToNiagara();
	PushDroneColorsToNiagara();
	PushDroneLabelAttributesToNiagara();

	if (bCountChanged)
	{
		// Burst count is evaluated during initialization, so only rebuild when fleet size changes.
		DroneFleetNiagara->ReinitializeSystem();
		bNiagaraArrayRebindPending = true;
	}
	else if (!DroneFleetNiagara->IsActive())
	{
		DroneFleetNiagara->Activate(true);
	}
	LastPushedDroneCount = NewDroneCount;
}

void ADroneFleetVisualizationActor::PushRouteTargetsToNiagara()
{
	if (!DroneFleetNiagara || !DroneFleetNiagara->GetAsset())
	{
		return;
	}

	// Route targets and primary drones are emitters in one Niagara system. Write
	// the complete state of both before a count change reinitializes that shared
	// system, otherwise its cold-start burst can observe a mixture of old and new
	// arrays. This is also used when targets are cleared or replaced.
	const int32 NewDroneCount = DronePositions.Num();
	const int32 NewRouteTargetCount = RouteTargetPositions.Num();
	const bool bDroneCountChanged = LastPushedDroneCount != NewDroneCount;
	const bool bRouteTargetCountChanged =
		LastPushedRouteTargetCount != NewRouteTargetCount;
	DroneFleetNiagara->SetVariableInt(DroneCountParameter, NewDroneCount);
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayPosition(
		DroneFleetNiagara,
		DronePositionsParameter,
		DronePositions);
	PushDroneColorsToNiagara();
	PushDroneLabelAttributesToNiagara();
	DroneFleetNiagara->SetVariableInt(RouteTargetCountParameter, NewRouteTargetCount);
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayPosition(
		DroneFleetNiagara,
		RouteTargetPositionsParameter,
		RouteTargetPositions);
	DroneFleetNiagara->SetVariableBool(
		RouteTargetsVisibleParameter,
		bRouteTargetsVisible
			&& VisualizationMode == EGCSVisualizationMode::Live
			&& NewRouteTargetCount > 0);

	if (bDroneCountChanged || bRouteTargetCountChanged)
	{
		// Both burst counts are evaluated during emitter initialization.
		DroneFleetNiagara->ReinitializeSystem();
		bNiagaraArrayRebindPending = true;
	}
	else if (!DroneFleetNiagara->IsActive())
	{
		DroneFleetNiagara->Activate(true);
	}
	LastPushedDroneCount = NewDroneCount;
	LastPushedRouteTargetCount = NewRouteTargetCount;
}

void ADroneFleetVisualizationActor::FlushDeferredNiagaraArrayRebind()
{
	if (!bNiagaraArrayRebindPending
		|| !DroneFleetNiagara
		|| !DroneFleetNiagara->GetAsset())
	{
		return;
	}

	// SetNiagaraArrayPosition converts world coordinates through the current
	// system instance's LWC converter. A count change replaces that instance, so
	// re-submit the complete dynamic state on the next actor tick, once the new
	// converter exists. LastPushed* already match, therefore this does not cause
	// another reinitialization and works for any live/NPZ particle count.
	bNiagaraArrayRebindPending = false;
	bDroneColorsDirty = true;
	bDroneLabelAttributesDirty = true;
	PushRouteTargetsToNiagara();

	UE_LOG(
		LogTemp,
		Verbose,
		TEXT("[DroneFleetViz] Deferred Niagara array rebind Drones=%d RouteTargets=%d Visible=%s"),
		DronePositions.Num(),
		RouteTargetPositions.Num(),
		bRouteTargetsVisible ? TEXT("YES") : TEXT("NO"));
}

void ADroneFleetVisualizationActor::HandleFleetChanged()
{
	RefreshFromFleet();
}

void ADroneFleetVisualizationActor::HandleDataBatchApplied(
	const int64 FleetRevision,
	const int32 UpdatedDroneCount)
{
	(void)FleetRevision;
	(void)UpdatedDroneCount;
	RefreshFromFleet();
}

bool ADroneFleetVisualizationActor::SetRoutePreviewFrame(
	const TArray<FVector3f>& PositionsENU,
	const TArray<FLinearColor>& Colors,
	const double OriginLatitude,
	const double OriginLongitude,
	const double OriginAltitude,
	FString& OutError)
{
	OutError.Reset();
	if (PositionsENU.IsEmpty() || PositionsENU.Num() != Colors.Num())
	{
		OutError = TEXT("Preview position/color arrays must be non-empty and have equal lengths.");
		return false;
	}
	if (!FMath::IsFinite(OriginLatitude) || OriginLatitude < -90.0 || OriginLatitude > 90.0
		|| !FMath::IsFinite(OriginLongitude) || OriginLongitude < -180.0 || OriginLongitude > 180.0
		|| !FMath::IsFinite(OriginAltitude))
	{
		OutError = TEXT("Preview geographic origin is invalid.");
		return false;
	}

	TArray<FVector> NewWorldPositions;
	FVector RouteOriginWorld = FVector::ZeroVector;
	FVector NewFleetVisualCenter = FVector::ZeroVector;
	if (!ConvertRouteENUPositionsToWorld(
		PositionsENU,
		OriginLatitude,
		OriginLongitude,
		OriginAltitude,
		NewWorldPositions,
		RouteOriginWorld,
		OutError,
		&NewFleetVisualCenter))
	{
		return false;
	}

	if (VisualizationMode == EGCSVisualizationMode::Live)
	{
		SuspendedLiveParticleDroneIDs = ParticleDroneIDs;
		SuspendedLiveDroneColors = DroneColors;
	}
	const bool bEnteringRoutePreview = VisualizationMode != EGCSVisualizationMode::RoutePreview;
	VisualizationMode = EGCSVisualizationMode::RoutePreview;
	if (bEnteringRoutePreview)
	{
		UpdatePrimaryDroneRendererVisibility();
	}
	SetRouteTargetsVisible(false);
	RoutePreviewOriginWorldPosition = RouteOriginWorld;
	bRoutePreviewOriginWorldPositionInitialized = true;
	ParticleDroneIDs.Reset();
	DronePositions = MoveTemp(NewWorldPositions);
	CachedFleetVisualCenter = NewFleetVisualCenter;
	bCachedFleetVisualCenterValid = true;
	DroneColors = Colors;
	for (FLinearColor& Color : DroneColors)
	{
		Color.R = FMath::Clamp(Color.R, 0.0f, 1.0f);
		Color.G = FMath::Clamp(Color.G, 0.0f, 1.0f);
		Color.B = FMath::Clamp(Color.B, 0.0f, 1.0f);
		Color.A = 1.0f;
	}
	bDroneColorsDirty = true;
	RebuildDroneLabelAttributes();
	PushArraysToNiagara();
	return true;
}

bool ADroneFleetVisualizationActor::ConvertRouteENUPositionsToWorld(
	const TArray<FVector3f>& PositionsENU,
	const double OriginLatitude,
	const double OriginLongitude,
	const double OriginAltitude,
	TArray<FVector>& OutWorldPositions,
	FVector& OutRouteOriginWorld,
	FString& OutError,
	FVector* OutWorldBoundsCenter)
{
	OutError.Reset();
	OutWorldPositions.Reset();
	OutRouteOriginWorld = FVector::ZeroVector;
	if (OutWorldBoundsCenter)
	{
		*OutWorldBoundsCenter = FVector::ZeroVector;
	}
	if (PositionsENU.IsEmpty())
	{
		OutError = TEXT("Route position array must be non-empty.");
		return false;
	}
	if (!FMath::IsFinite(OriginLatitude) || OriginLatitude < -90.0 || OriginLatitude > 90.0
		|| !FMath::IsFinite(OriginLongitude) || OriginLongitude < -180.0 || OriginLongitude > 180.0
		|| !FMath::IsFinite(OriginAltitude))
	{
		OutError = TEXT("Preview geographic origin is invalid.");
		return false;
	}

	CacheCesiumGeoreference();
	ACesiumGeoreference* Georeference = CesiumGeoreference.Get();
	if (!Georeference)
	{
		OutError = TEXT("CesiumGeoreference is unavailable for route preview conversion.");
		return false;
	}

	const FVector OriginInGeoreference =
		Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(
			OriginLongitude,
			OriginLatitude,
			OriginAltitude));
	const FMatrix EastSouthUpToGeoreference =
		Georeference->ComputeEastSouthUpToUnrealTransformation(OriginInGeoreference);
	const double UnrealUnitsPerMeter = Georeference->GetScale();
	const FTransform GeoreferenceActorTransform = Georeference->GetActorTransform();
	OutRouteOriginWorld =
		GeoreferenceActorTransform.TransformPosition(OriginInGeoreference);
	if (OutRouteOriginWorld.ContainsNaN())
	{
		OutRouteOriginWorld = FVector::ZeroVector;
		OutError = TEXT("Cesium conversion failed for the route ShowOrigin.");
		return false;
	}

	OutWorldPositions.SetNumUninitialized(PositionsENU.Num());
	FBox WorldBounds(ForceInit);
	for (int32 RouteSlotIndex = 0; RouteSlotIndex < PositionsENU.Num(); ++RouteSlotIndex)
	{
		const FVector3f LocalENU = PositionsENU[RouteSlotIndex];
		if (LocalENU.ContainsNaN())
		{
			OutWorldPositions.Reset();
			OutError = FString::Printf(
				TEXT("RouteSlotIndex %d has a non-finite position."),
				RouteSlotIndex);
			return false;
		}

		// Route X/Y/Z are East/North/Up meters. Cesium's tangent frame is
		// East/South/Up in Unreal units, hence the single North sign inversion.
		const FVector LocalESUUnrealUnits(
			static_cast<double>(LocalENU.X) * UnrealUnitsPerMeter,
			-static_cast<double>(LocalENU.Y) * UnrealUnitsPerMeter,
			static_cast<double>(LocalENU.Z) * UnrealUnitsPerMeter);
		const FVector PositionInGeoreference =
			EastSouthUpToGeoreference.TransformPosition(LocalESUUnrealUnits);
		const FVector WorldPosition =
			GeoreferenceActorTransform.TransformPosition(PositionInGeoreference);
		if (WorldPosition.ContainsNaN())
		{
			OutWorldPositions.Reset();
			OutError = FString::Printf(
				TEXT("Cesium conversion failed for RouteSlotIndex %d."),
				RouteSlotIndex);
			return false;
		}
		OutWorldPositions[RouteSlotIndex] = WorldPosition;
		WorldBounds += WorldPosition;
	}
	if (OutWorldBoundsCenter && WorldBounds.IsValid)
	{
		*OutWorldBoundsCenter = WorldBounds.GetCenter();
	}
	return true;
}

bool ADroneFleetVisualizationActor::ConvertRouteENUPositionsToLiveWorld(
	const TArray<FVector3f>& PositionsENU,
	const double OriginLatitude,
	const double OriginLongitude,
	const double OriginAltitude,
	TArray<FVector>& OutWorldPositions,
	FVector& OutRouteOriginWorld,
	FString& OutError,
	FVector* OutWorldBoundsCenter)
{
	if (!bOriginWorldPositionInitialized
		|| OriginWorldPosition.ContainsNaN())
	{
		OutWorldPositions.Reset();
		OutRouteOriginWorld = FVector::ZeroVector;
		if (OutWorldBoundsCenter)
		{
			*OutWorldBoundsCenter = FVector::ZeroVector;
		}
		OutError = TEXT("Live fleet display origin is unavailable for route overlay conversion.");
		return false;
	}

	if (!ConvertRouteENUPositionsToWorld(
		PositionsENU,
		OriginLatitude,
		OriginLongitude,
		OriginAltitude,
		OutWorldPositions,
		OutRouteOriginWorld,
		OutError,
		OutWorldBoundsCenter))
	{
		return false;
	}

	// The imported ShowOrigin drone and the fixed-pile drone are one physical
	// drone. Therefore NPZ ENU (0,0,0) is the Live display anchor, even when the
	// stable GPS captured for ShowOrigin differs slightly from the most recent
	// telemetry sample used to lock FleetGeoOrigin. Rebase from the converted
	// route origin itself instead of retaining that sampling delta.
	//
	// Passing absolute Cesium coordinates to User.RouteTargetPositions while
	// User.DronePositions is display-anchored would also mix two coordinate
	// spaces in one Niagara system and lose LWC precision.
	const FVector AbsoluteRouteOriginWorld = OutRouteOriginWorld;
	const FVector LiveDisplayTranslation =
		OriginWorldPosition - AbsoluteRouteOriginWorld;
	for (FVector& WorldPosition : OutWorldPositions)
	{
		WorldPosition += LiveDisplayTranslation;
	}
	OutRouteOriginWorld = OriginWorldPosition;
	if (OutWorldBoundsCenter)
	{
		*OutWorldBoundsCenter += LiveDisplayTranslation;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RouteOverlay] Live-space conversion Targets=%d Translation=(%.3f,%.3f,%.3f) Origin=(%.3f,%.3f,%.3f)"),
		OutWorldPositions.Num(),
		LiveDisplayTranslation.X,
		LiveDisplayTranslation.Y,
		LiveDisplayTranslation.Z,
		OutRouteOriginWorld.X,
		OutRouteOriginWorld.Y,
		OutRouteOriginWorld.Z);
	return true;
}

void ADroneFleetVisualizationActor::ExitRoutePreview()
{
	if (VisualizationMode != EGCSVisualizationMode::RoutePreview)
	{
		return;
	}

	VisualizationMode = EGCSVisualizationMode::Live;
	LivePositionStates.Reset();
	UpdatePrimaryDroneRendererVisibility();
	bRoutePreviewOriginWorldPositionInitialized = false;
	RoutePreviewOriginWorldPosition = FVector::ZeroVector;
	ParticleDroneIDs = MoveTemp(SuspendedLiveParticleDroneIDs);
	DroneColors = MoveTemp(SuspendedLiveDroneColors);
	bDroneColorsDirty = true;
	RefreshFromFleet();
}
