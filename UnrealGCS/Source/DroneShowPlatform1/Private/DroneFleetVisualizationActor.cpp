/**
 * @brief 实现 Fleet -> WGS84/Cesium -> UE World Position -> Niagara 数组的数据桥。
 *
 * 分类：Production。
 * Offline 只改变状态，仍保留粒子和 DroneOrder -> ParticleIndex；无效 GPS 使用 ZeroVector。
 */
#include "DroneFleetVisualizationActor.h"

#include "Camera/PlayerCameraManager.h"
#include "CesiumGeoreference.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "FleetManagerSubsystem.h"
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
}

ADroneFleetVisualizationActor::ADroneFleetVisualizationActor()
{
	PrimaryActorTick.bCanEverTick = false;
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
	SetDroneLabelsVisible(false);
	BindFleetManager();
	if (!FleetManager.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DroneFleetViz] FleetManager is unavailable; actor will remain in the level."));
		return;
	}

	RefreshFromFleet();
}

void ADroneFleetVisualizationActor::SetDroneLabelsVisible(const bool bVisible)
{
	if (DroneFleetNiagara)
	{
		DroneFleetNiagara->SetVariableBool(DroneLabelsVisibleParameter, bVisible);
	}
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
	if (Fleet)
	{
		// GetDronePage 保留 Fleet DroneOrder，因此 ParticleIndex -> DroneID 映射稳定。
		// 不得按 TMap、GPS、Online 或选择状态重排；Offline 也必须保留原索引。
		OrderedDrones = Fleet->GetDronePage(0, Fleet->GetDroneCount());
		NewParticleDroneIDs.Reserve(OrderedDrones.Num());
		NewDroneColors.Reserve(OrderedDrones.Num());
		DronePositions.SetNumZeroed(OrderedDrones.Num());

		const FFleetGeoOrigin FleetGeoOrigin = Fleet->GetFleetGeoOrigin();
		UpdateOriginWorldPosition(FleetGeoOrigin);
		for (int32 Index = 0; Index < OrderedDrones.Num(); ++Index)
		{
			const FDroneData& Drone = OrderedDrones[Index];
			NewParticleDroneIDs.Add(Drone.DroneID);
			const FLinearColor* ExistingColor = ExistingColorsByDroneID.Find(Drone.DroneID);
			NewDroneColors.Add(ExistingColor ? *ExistingColor : Drone.LEDColor);

			// FleetGeoOrigin 尚未建立时，任何后续无人机都不能抢占逻辑原点。
			// 单机 GPS 无效时必须回到 ZeroVector，禁止保留历史坐标或使用索引占位。
			if (FleetGeoOrigin.bOriginInitialized && DroneGPS::IsValid(Drone))
			{
				DronePositions[Index] = ConvertDroneGPSPosition(Drone, FleetGeoOrigin);
			}
		}

		// Offline 只改变状态，不删除粒子，也不改变 DroneOrder -> ParticleIndex 映射。
	}

	if (ParticleDroneIDs != NewParticleDroneIDs
		|| DroneColors.Num() != NewDroneColors.Num()
		|| DroneDisplayIDs.Num() != NewParticleDroneIDs.Num()
		|| DroneSelectedMasks.Num() != NewParticleDroneIDs.Num())
	{
		ParticleDroneIDs = MoveTemp(NewParticleDroneIDs);
		DroneColors = MoveTemp(NewDroneColors);
		bDroneColorsDirty = true;
		RebuildDroneLabelAttributes();
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
				TEXT("[DroneFleetViz Debug] CameraWorld=(%.3f,%.3f,%.3f) CameraToDrone001=%.3fcm (%.3fkm)"),
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

void ADroneFleetVisualizationActor::UpdateOriginWorldPosition(const FFleetGeoOrigin& FleetGeoOrigin)
{
	if (!FleetGeoOrigin.bOriginInitialized)
	{
		bOriginWorldPositionInitialized = false;
		OriginWorldPosition = FVector::ZeroVector;
		CachedOriginDroneID.Reset();
		return;
	}

	ACesiumGeoreference* Georeference = CesiumGeoreference.Get();
	if (!Georeference)
	{
		bOriginWorldPositionInitialized = false;
		OriginWorldPosition = FVector::ZeroVector;
		return;
	}

	const bool bSameLockedOrigin = bOriginWorldPositionInitialized
		&& CachedOriginDroneID == FleetGeoOrigin.OriginDroneID
		&& CachedOriginLatitude == FleetGeoOrigin.OriginLatitude
		&& CachedOriginLongitude == FleetGeoOrigin.OriginLongitude
		&& CachedOriginAltitudeAMSL == FleetGeoOrigin.OriginAltitudeAMSL
		&& bCachedOriginAltitudeValid == FleetGeoOrigin.bOriginAltitudeValid;
	if (bSameLockedOrigin)
	{
		return;
	}

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
	OriginWorldPosition = Georeference->GetActorTransform().TransformPosition(GeoreferencePosition);
	if (OriginWorldPosition.ContainsNaN())
	{
		OriginWorldPosition = FVector::ZeroVector;
		bOriginWorldPositionInitialized = false;
		return;
	}

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
	const FVector WorldPosition =
		Georeference->GetActorTransform().TransformPosition(GeoreferencePosition);
	return WorldPosition.ContainsNaN() ? FVector::ZeroVector : WorldPosition;
}

void ADroneFleetVisualizationActor::RebuildDroneLabelAttributes()
{
	const int32 ParticleCount = DronePositions.Num();
	DroneDisplayIDs.SetNumUninitialized(ParticleCount);
	DroneSelectedMasks.SetNumUninitialized(ParticleCount);
	for (int32 Index = 0; Index < ParticleCount; ++Index)
	{
		DroneDisplayIDs[Index] = static_cast<float>(Index + 1);
		DroneSelectedMasks[Index] = VisualizationMode == EGCSVisualizationMode::Live
			&& ParticleDroneIDs.IsValidIndex(Index)
			&& CommandSelectedDroneIDs.Contains(ParticleDroneIDs[Index])
			? 1.0f
			: 0.0f;
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

	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayColor(
		DroneFleetNiagara,
		DroneColorsParameter,
		DroneColors);
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
	UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayPosition(
		DroneFleetNiagara,
		DronePositionsParameter,
		DronePositions);
	PushDroneColorsToNiagara();
	PushDroneLabelAttributesToNiagara();

	if (bCountChanged)
	{
		// Burst count is evaluated during initialization, so only rebuild when fleet size changes.
		DroneFleetNiagara->ReinitializeSystem();
	}
	else if (!DroneFleetNiagara->IsActive())
	{
		DroneFleetNiagara->Activate(true);
	}
	LastPushedDroneCount = NewDroneCount;
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

	TArray<FVector> NewWorldPositions;
	NewWorldPositions.SetNumUninitialized(PositionsENU.Num());
	for (int32 RouteSlotIndex = 0; RouteSlotIndex < PositionsENU.Num(); ++RouteSlotIndex)
	{
		const FVector3f LocalENU = PositionsENU[RouteSlotIndex];
		if (LocalENU.ContainsNaN())
		{
			OutError = FString::Printf(TEXT("RouteSlotIndex %d has a non-finite position."), RouteSlotIndex);
			return false;
		}

		// Cesium's local horizontal frame is East-South-Up and its Unreal-local
		// units use Georeference scale (normally 100 units per meter).
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
			OutError = FString::Printf(TEXT("Cesium conversion failed for RouteSlotIndex %d."), RouteSlotIndex);
			return false;
		}
		NewWorldPositions[RouteSlotIndex] = WorldPosition;
	}

	if (VisualizationMode == EGCSVisualizationMode::Live)
	{
		SuspendedLiveParticleDroneIDs = ParticleDroneIDs;
		SuspendedLiveDroneColors = DroneColors;
	}
	VisualizationMode = EGCSVisualizationMode::RoutePreview;
	ParticleDroneIDs.Reset();
	DronePositions = MoveTemp(NewWorldPositions);
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

void ADroneFleetVisualizationActor::ExitRoutePreview()
{
	if (VisualizationMode != EGCSVisualizationMode::RoutePreview)
	{
		return;
	}

	VisualizationMode = EGCSVisualizationMode::Live;
	ParticleDroneIDs = MoveTemp(SuspendedLiveParticleDroneIDs);
	DroneColors = MoveTemp(SuspendedLiveDroneColors);
	bDroneColorsDirty = true;
	RefreshFromFleet();
}
