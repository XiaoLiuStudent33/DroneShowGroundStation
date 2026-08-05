#include "FleetManagerSubsystem.h"

void UFleetManagerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UFleetManagerSubsystem::Deinitialize()
{
	DronesByID.Reset();
	DroneOrder.Reset();
	SelectedDroneID.Reset();
	FleetRevision = 0;
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
	if (SelectedDroneID.Equals(DroneID, ESearchCase::IgnoreCase))
	{
		SelectedDroneID.Reset();
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

	if (FDroneData* PreviousSelection = DronesByID.Find(SelectedDroneID))
	{
		PreviousSelection->bSelected = false;
		OnDroneUpdated.Broadcast(*PreviousSelection);
	}

	SelectedDroneID = DroneID;
	NewSelection->bSelected = true;
	OnDroneUpdated.Broadcast(*NewSelection);
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

	FString NewSelectedDroneID;
	if (NewDronesByID.Contains(SelectedDroneID))
	{
		NewSelectedDroneID = SelectedDroneID;
	}
	else if (NewDronesByID.Contains(SnapshotSelectedDroneID))
	{
		NewSelectedDroneID = SnapshotSelectedDroneID;
	}

	DronesByID = MoveTemp(NewDronesByID);
	DroneOrder = MoveTemp(NewDroneOrder);
	SelectedDroneID = MoveTemp(NewSelectedDroneID);
	if (FDroneData* SelectedDrone = DronesByID.Find(SelectedDroneID))
	{
		SelectedDrone->bSelected = true;
	}

	FleetRevision = Snapshot.FleetRevision > 0
		? Snapshot.FleetRevision
		: FleetRevision + 1;
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

bool UFleetManagerSubsystem::GetSelectedDrone(FDroneData& OutDroneData) const
{
	const FDroneData* SelectedDrone = DronesByID.Find(SelectedDroneID);

	if (!SelectedDrone)
	{
		return false;
	}

	OutDroneData = *SelectedDrone;
	return true;
}
