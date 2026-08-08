/**
 * @brief 实现跨线程 Snapshot/Delta 合并并调度到 GameThread。
 *
 * 分类：Production。
 * 调用链：Data Provider -> pending batch -> GameThread Flush -> FleetManager。
 */
#include "GCSServerAdapterSubsystem.h"

#include "Async/Async.h"
#include "FleetManagerSubsystem.h"
#include "GCSDataProvider.h"
#include "Misc/ScopeLock.h"

void UGCSServerAdapterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UFleetManagerSubsystem>();
}

void UGCSServerAdapterSubsystem::Deinitialize()
{
	for (IGCSDataProvider* Provider : Providers)
	{
		if (Provider)
		{
			Provider->Stop();
		}
	}
	Providers.Reset();

	{
		FScopeLock Lock(&PendingDataMutex);
		PendingSnapshot.Reset();
		PendingDeltasByID.Reset();
		PendingDeltaOrder.Reset();
		bFlushScheduled = false;
	}

	Super::Deinitialize();
}

bool UGCSServerAdapterSubsystem::RegisterProvider(IGCSDataProvider& Provider)
{
	for (const IGCSDataProvider* ExistingProvider : Providers)
	{
		if (ExistingProvider == &Provider
			|| (ExistingProvider && ExistingProvider->GetProviderID() == Provider.GetProviderID()))
		{
			return false;
		}
	}

	if (!Provider.Start(*this))
	{
		return false;
	}

	Providers.Add(&Provider);
	return true;
}

void UGCSServerAdapterSubsystem::UnregisterProvider(IGCSDataProvider& Provider)
{
	if (Providers.RemoveSingle(&Provider) > 0)
	{
		Provider.Stop();
	}
}

bool UGCSServerAdapterSubsystem::PushFleetSnapshot(const FGCSFleetSnapshot& Snapshot)
{
	if (!ValidateHeader(Snapshot.Header))
	{
		return false;
	}

	{
		FScopeLock Lock(&PendingDataMutex);
		PendingSnapshot = Snapshot;
		PendingDeltasByID.Reset();
		PendingDeltaOrder.Reset();
		PendingDeltaFleetRevision = Snapshot.FleetRevision;
	}

	ScheduleFlush();
	return true;
}

bool UGCSServerAdapterSubsystem::PushTelemetryBatch(const FGCSTelemetryBatch& Batch)
{
	if (!ValidateHeader(Batch.Header))
	{
		return false;
	}

	{
		FScopeLock Lock(&PendingDataMutex);
		for (const FDroneDataDelta& Delta : Batch.Deltas)
		{
			if (Delta.DroneID.IsEmpty())
			{
				continue;
			}

			if (FDroneDataDelta* ExistingDelta = PendingDeltasByID.Find(Delta.DroneID))
			{
				GCSDataDelta::Merge(*ExistingDelta, Delta);
				continue;
			}

			if (PendingDeltasByID.Num() >= MaxPendingDroneDeltas)
			{
				UE_LOG(
					LogTemp,
					Warning,
					TEXT("GCS data adapter rejected delta because the bounded queue is full."));
				continue;
			}

			PendingDeltasByID.Add(Delta.DroneID, Delta);
			PendingDeltaOrder.Add(Delta.DroneID);
		}

		PendingDeltaHeader = Batch.Header;
		PendingDeltaFleetRevision = FMath::Max(PendingDeltaFleetRevision, Batch.FleetRevision);
	}

	ScheduleFlush();
	return true;
}

void UGCSServerAdapterSubsystem::ScheduleFlush()
{
	bool bShouldSchedule = false;
	{
		FScopeLock Lock(&PendingDataMutex);
		if (!bFlushScheduled)
		{
			bFlushScheduled = true;
			bShouldSchedule = true;
		}
	}

	if (!bShouldSchedule)
	{
		return;
	}

	// 网络接收/协议解析不直接触碰 Fleet 或 UI；这里只安排一次 GameThread
	// Flush，把当前积累的数据批量交给 FleetManager 原子应用。
	const TWeakObjectPtr<UGCSServerAdapterSubsystem> WeakThis(this);
	AsyncTask(
		ENamedThreads::GameThread,
		[WeakThis]()
		{
			if (UGCSServerAdapterSubsystem* Adapter = WeakThis.Get())
			{
				Adapter->FlushPendingData();
			}
		});
}

void UGCSServerAdapterSubsystem::FlushPendingData()
{
	check(IsInGameThread());

	TOptional<FGCSFleetSnapshot> Snapshot;
	FGCSTelemetryBatch DeltaBatch;
	{
		FScopeLock Lock(&PendingDataMutex);
		Snapshot = MoveTemp(PendingSnapshot);
		PendingSnapshot.Reset();

		DeltaBatch.Header = PendingDeltaHeader;
		DeltaBatch.FleetRevision = PendingDeltaFleetRevision;
		DeltaBatch.Deltas.Reserve(PendingDeltaOrder.Num());
		for (const FString& DroneID : PendingDeltaOrder)
		{
			if (FDroneDataDelta* Delta = PendingDeltasByID.Find(DroneID))
			{
				DeltaBatch.Deltas.Add(MoveTemp(*Delta));
			}
		}

		PendingDeltasByID.Reset();
		PendingDeltaOrder.Reset();
		PendingDeltaFleetRevision = 0;
		bFlushScheduled = false;
	}

	UFleetManagerSubsystem* Fleet =
		GetGameInstance()->GetSubsystem<UFleetManagerSubsystem>();
	if (!Fleet)
	{
		return;
	}

	if (Snapshot.IsSet())
	{
		Fleet->ApplySnapshot(Snapshot.GetValue());
	}

	if (!DeltaBatch.Deltas.IsEmpty())
	{
		Fleet->ApplyDeltaBatch(DeltaBatch);
	}
}

bool UGCSServerAdapterSubsystem::ValidateHeader(const FGCSMessageHeader& Header) const
{
	return Header.SchemaVersion == GCSDataSchema::CurrentVersion
		&& !Header.SourceID.IsEmpty();
}
