/**
 * @brief 实现 Fleet Snapshot/Delta 的原子应用、稳定顺序和详情选择保持。
 *
 * 分类：Production。
 * 调用链：GCSServerAdapter -> FleetManager -> GCSUIBridge/DroneFleetVisualizationActor。
 * Offline Drone 仍存在于 Snapshot/DroneOrder 中，不因在线状态变化被移除或重排。
 */
#include "FleetManagerSubsystem.h"

void UFleetManagerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ResetGeoOrigin();
}

void UFleetManagerSubsystem::Deinitialize()
{
	DronesByID.Reset();
	DroneOrder.Reset();
	CurrentSelectedDroneID.Reset();
	FleetRevision = 0;
	ActiveFleetSessionID.Reset();
	ResetGeoOrigin();
	Super::Deinitialize();
}

bool UFleetManagerSubsystem::AddDrone(const FDroneData& DroneData)
{
	if (DroneData.DroneID.IsEmpty() || DronesByID.Contains(DroneData.DroneID))
	{
		return false;
	}

	DronesByID.Add(DroneData.DroneID, DroneData);
	DroneOrder.Add(DroneData.DroneID);
	TryInitializeGeoOrigin();
	OnFleetChanged.Broadcast();
	return true;
}

bool UFleetManagerSubsystem::RemoveDrone(const FString& DroneID)
{
	if (DronesByID.Remove(DroneID) == 0)
	{
		return false;
	}

	DroneOrder.RemoveSingle(DroneID);
	if (CurrentSelectedDroneID.Equals(DroneID, ESearchCase::IgnoreCase))
	{
		CurrentSelectedDroneID.Reset();
	}
	OnFleetChanged.Broadcast();
	return true;
}

bool UFleetManagerSubsystem::UpdateDrone(const FDroneData& DroneData)
{
	FDroneData* ExistingDrone = DronesByID.Find(DroneData.DroneID);
	if (!ExistingDrone)
	{
		return false;
	}

	const bool bWasSelected = ExistingDrone->bSelected;
	*ExistingDrone = DroneData;
	ExistingDrone->bSelected = bWasSelected;
	TryInitializeGeoOrigin();
	OnDroneUpdated.Broadcast(*ExistingDrone);
	return true;
}

bool UFleetManagerSubsystem::SelectDrone(const FString& DroneID)
{
	FDroneData* NewSelection = DronesByID.Find(DroneID);
	if (!NewSelection)
	{
		return false;
	}

	if (CurrentSelectedDroneID.Equals(DroneID, ESearchCase::IgnoreCase))
	{
		NewSelection->bSelected = true;
		OnDroneSelected.Broadcast(CurrentSelectedDroneID);
		return true;
	}

	if (FDroneData* PreviousSelection = DronesByID.Find(CurrentSelectedDroneID))
	{
		PreviousSelection->bSelected = false;
		OnDroneUpdated.Broadcast(*PreviousSelection);
	}

	CurrentSelectedDroneID = DroneID;
	NewSelection->bSelected = true;
	OnDroneUpdated.Broadcast(*NewSelection);
	OnDroneSelected.Broadcast(CurrentSelectedDroneID);
	return true;
}

bool UFleetManagerSubsystem::ApplySnapshot(const FGCSFleetSnapshot& Snapshot)
{
	check(IsInGameThread());

	if (Snapshot.Header.SchemaVersion != GCSDataSchema::CurrentVersion)
	{
		return false;
	}

	TMap<FString, FDroneData> NewDronesByID;
	TArray<FString> NewDroneOrder;
	NewDronesByID.Reserve(Snapshot.Drones.Num());
	NewDroneOrder.Reserve(Snapshot.Drones.Num());

	FString SnapshotSelectedDroneID;
	for (const FDroneData& InputDrone : Snapshot.Drones)
	{
		if (InputDrone.DroneID.IsEmpty() || NewDronesByID.Contains(InputDrone.DroneID))
		{
			return false;
		}

		FDroneData Drone = InputDrone;
		if (Drone.bSelected && SnapshotSelectedDroneID.IsEmpty())
		{
			SnapshotSelectedDroneID = Drone.DroneID;
		}
		Drone.bSelected = false;
		NewDroneOrder.Add(Drone.DroneID);
		NewDronesByID.Add(Drone.DroneID, MoveTemp(Drone));
	}

	if (!Snapshot.Header.SessionID.IsEmpty())
	{
		if (!ActiveFleetSessionID.IsEmpty()
			&& !ActiveFleetSessionID.Equals(Snapshot.Header.SessionID, ESearchCase::CaseSensitive))
		{
			ResetGeoOrigin();
		}
		ActiveFleetSessionID = Snapshot.Header.SessionID;
	}

	FString NewSelectedDroneID;
	// CurrentSelectedDroneID 只用于详情查看/高亮；多机命令勾选由 UI Bridge 的
	// SelectedCommandDroneIDs 独立维护，两种选择状态不能互相覆盖。
	if (NewDronesByID.Contains(CurrentSelectedDroneID))
	{
		NewSelectedDroneID = CurrentSelectedDroneID;
	}
	else if (NewDronesByID.Contains(SnapshotSelectedDroneID))
	{
		NewSelectedDroneID = SnapshotSelectedDroneID;
	}

	// Snapshot 顺序来自 Console 稳定 SessionOrder。整批替换后保持
	// DroneOrder[Index] -> Niagara ParticleIndex 映射，Offline 不单独重排。
	DronesByID = MoveTemp(NewDronesByID);
	DroneOrder = MoveTemp(NewDroneOrder);
	CurrentSelectedDroneID = MoveTemp(NewSelectedDroneID);
	if (FDroneData* SelectedDrone = DronesByID.Find(CurrentSelectedDroneID))
	{
		SelectedDrone->bSelected = true;
	}
	TryInitializeGeoOrigin();

	FleetRevision = Snapshot.FleetRevision > 0
		? Snapshot.FleetRevision
		: FleetRevision + 1;
#if WITH_DEV_AUTOMATION_TESTS
	++DataBatchAppliedBroadcastCountForTests;
#endif
	OnDataBatchApplied.Broadcast(FleetRevision, DronesByID.Num());
	return true;
}

bool UFleetManagerSubsystem::ApplyDeltaBatch(const FGCSTelemetryBatch& Batch)
{
	check(IsInGameThread());

	if (Batch.Header.SchemaVersion != GCSDataSchema::CurrentVersion
		|| (Batch.FleetRevision > 0 && Batch.FleetRevision < FleetRevision))
	{
		return false;
	}

	int32 UpdatedDroneCount = 0;
	for (const FDroneDataDelta& Delta : Batch.Deltas)
	{
		if (Delta.DroneID.IsEmpty())
		{
			continue;
		}

		FDroneData* Drone = DronesByID.Find(Delta.DroneID);
		if (!Drone && Delta.bCreateIfMissing)
		{
			FDroneData NewDrone;
			NewDrone.DroneID = Delta.DroneID;
			DronesByID.Add(Delta.DroneID, NewDrone);
			DroneOrder.Add(Delta.DroneID);
			Drone = DronesByID.Find(Delta.DroneID);
		}

		if (!Drone)
		{
			continue;
		}

		GCSDataDelta::ApplyTo(*Drone, Delta);
		Drone->DroneID = Delta.DroneID;
		++UpdatedDroneCount;
	}

	if (UpdatedDroneCount == 0)
	{
		return false;
	}

	if (Batch.FleetRevision > 0)
	{
		FleetRevision = FMath::Max(FleetRevision, Batch.FleetRevision);
	}
	TryInitializeGeoOrigin();
#if WITH_DEV_AUTOMATION_TESTS
	++DataBatchAppliedBroadcastCountForTests;
#endif
	OnDataBatchApplied.Broadcast(FleetRevision, UpdatedDroneCount);
	return true;
}

TArray<FDroneData> UFleetManagerSubsystem::GetDronePage(
	const int32 Offset,
	const int32 Limit) const
{
	TArray<FDroneData> Result;
	if (Offset < 0 || Limit <= 0 || Offset >= DroneOrder.Num())
	{
		return Result;
	}

	const int32 EndIndex = FMath::Min(Offset + Limit, DroneOrder.Num());
	Result.Reserve(EndIndex - Offset);
	for (int32 Index = Offset; Index < EndIndex; ++Index)
	{
		if (const FDroneData* DroneData = DronesByID.Find(DroneOrder[Index]))
		{
			Result.Add(*DroneData);
		}
	}

	return Result;
}

int32 UFleetManagerSubsystem::GetDroneCount() const
{
	return DronesByID.Num();
}

bool UFleetManagerSubsystem::GetDroneByID(
	const FString& DroneID,
	FDroneData& OutDroneData) const
{
	const FDroneData* DroneData = DronesByID.Find(DroneID);
	if (!DroneData)
	{
		return false;
	}

	OutDroneData = *DroneData;
	return true;
}

bool UFleetManagerSubsystem::SetGeoOriginFromDroneID(
	const FString& DroneID,
	FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();

	const FDroneData* OriginDrone = DronesByID.Find(DroneID);
	if (!OriginDrone)
	{
		OutError = FString::Printf(TEXT("无人机 %s 不存在"), *DroneID);
		return false;
	}
	if (!OriginDrone->bOnline)
	{
		OutError = FString::Printf(TEXT("无人机 %s 当前离线"), *DroneID);
		return false;
	}
	if (!DroneGPS::IsValid(*OriginDrone))
	{
		OutError = FString::Printf(TEXT("无人机 %s 没有有效的 3D GPS 定位"), *DroneID);
		return false;
	}

	FleetGeoOrigin.bOriginInitialized = true;
	FleetGeoOrigin.OriginDroneID = OriginDrone->DroneID;
	FleetGeoOrigin.OriginLatitude = OriginDrone->GPSLat;
	FleetGeoOrigin.OriginLongitude = OriginDrone->GPSLon;
	FleetGeoOrigin.bOriginAltitudeValid = OriginDrone->bAltitudeValid;
	FleetGeoOrigin.OriginAltitudeAMSL = OriginDrone->bAltitudeValid
		? OriginDrone->AltitudeAMSL
		: 0.0f;

	OnFleetChanged.Broadcast();
	return true;
}

bool UFleetManagerSubsystem::GetSelectedDrone(FDroneData& OutDroneData) const
{
	const FDroneData* SelectedDrone = DronesByID.Find(CurrentSelectedDroneID);

	if (!SelectedDrone)
	{
		return false;
	}

	OutDroneData = *SelectedDrone;
	return true;
}

void UFleetManagerSubsystem::TryInitializeGeoOrigin()
{
	if (FleetGeoOrigin.bOriginInitialized || DroneOrder.IsEmpty())
	{
		return;
	}

	// Deterministic rule: only the first drone in fleet logical order may anchor
	// this session. A later drone with valid GPS must wait for this one.
	const FString& OriginCandidateID = DroneOrder[0];
	const FDroneData* OriginCandidate = DronesByID.Find(OriginCandidateID);
	if (!OriginCandidate || !DroneGPS::IsValid(*OriginCandidate))
	{
		return;
	}

	FleetGeoOrigin.bOriginInitialized = true;
	FleetGeoOrigin.OriginDroneID = OriginCandidate->DroneID;
	FleetGeoOrigin.OriginLatitude = OriginCandidate->GPSLat;
	FleetGeoOrigin.OriginLongitude = OriginCandidate->GPSLon;
	FleetGeoOrigin.bOriginAltitudeValid = OriginCandidate->bAltitudeValid;
	FleetGeoOrigin.OriginAltitudeAMSL = OriginCandidate->bAltitudeValid
		? OriginCandidate->AltitudeAMSL
		: 0.0f;
}

void UFleetManagerSubsystem::ResetGeoOrigin()
{
	FleetGeoOrigin = FFleetGeoOrigin{};
}
