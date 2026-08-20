#include "RouteAssignmentSubsystem.h"

#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING
namespace DroneGCSRouteAssignmentConsolePrivate
{
	void BindExplicit(const TArray<FString>& Args, UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		URouteAssignmentSubsystem* Assignment = GameInstance
			? GameInstance->GetSubsystem<URouteAssignmentSubsystem>() : nullptr;
		UDSPGCSFrozenAssignmentSyncSubsystem* Sync = GameInstance
			? GameInstance->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>() : nullptr;
		int32 RouteSlotIndex = INDEX_NONE;
		if (!Assignment || !Sync || Args.Num() != 2 || Args[0].IsEmpty()
			|| !LexTryParseString(RouteSlotIndex, *Args[1])
			|| RouteSlotIndex < 0)
		{
			UE_LOG(LogTemp, Error,
				TEXT("Usage: RouteAssignment.BindExplicit <exact-DroneID> <zero-based-RouteSlotIndex>"));
			return;
		}
		if (Assignment->IsFrozen())
		{
			UE_LOG(LogTemp, Error,
				TEXT("[RouteAssignment] Explicit binding failed: existing assignment is Frozen; reset it explicitly first."));
			return;
		}

		FRouteAssignmentSourceSnapshot Source;
		FString Error;
		if (!Assignment->CaptureSourceSnapshot(Source, Error))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[RouteAssignment] Explicit binding failed: %s"), *Error);
			return;
		}

		FRouteAssignmentExplicitBindingRequest Request;
		Request.ExpectedSourceIdentity = Source.SourceIdentity;
		FRouteAssignmentExplicitBinding& Binding = Request.Bindings.AddDefaulted_GetRef();
		Binding.DroneID = Args[0];
		Binding.RouteSlotIndex = RouteSlotIndex;
		if (!Assignment->BuildAndReplaceExplicitWorkingAssignment(Request, Error))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[RouteAssignment] Explicit binding failed: %s"), *Error);
			return;
		}
		UE_LOG(LogTemp, Display,
			TEXT("[RouteAssignment] Explicit binding accepted DroneID=%s RouteSlot=%d"),
			*Args[0], RouteSlotIndex);

		if (!Assignment->Freeze(ERouteAssignmentFreezePolicy::AllowDegraded, Error))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[RouteAssignment] Frozen failed: %s"), *Error);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("[RouteAssignment] Frozen (AllowDegraded)"));
		if (!Sync->StartSyncFrozenAssignment(Error))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[RouteAssignment] Sync failed to start: %s"), *Error);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("[RouteAssignment] Syncing"));

		const double Deadline = FPlatformTime::Seconds() + 30.0;
		TWeakObjectPtr<UDSPGCSFrozenAssignmentSyncSubsystem> WeakSync(Sync);
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WeakSync, Deadline](float)
			{
				const UDSPGCSFrozenAssignmentSyncSubsystem* Current = WeakSync.Get();
				if (!Current)
				{
					UE_LOG(LogTemp, Error,
						TEXT("[RouteAssignment] Sync Failed: subsystem unavailable."));
					return false;
				}
				const FDSPGCSFrozenAssignmentSyncStatus Status = Current->GetSyncStatus();
				if (Status.State == EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted)
				{
					UE_LOG(LogTemp, Display,
						TEXT("[RouteAssignment] ConsoleAccepted"));
					return false;
				}
				if (Status.State == EDSPGCSFrozenAssignmentSyncState::SyncFailed)
				{
					UE_LOG(LogTemp, Error,
						TEXT("[RouteAssignment] Sync Failed: %s"), *Status.LastError);
					return false;
				}
				if (FPlatformTime::Seconds() >= Deadline)
				{
					UE_LOG(LogTemp, Error,
						TEXT("[RouteAssignment] Sync Failed: bounded console-command monitor timed out."));
					return false;
				}
				return true;
			}),
			0.1f);
	}

	FAutoConsoleCommandWithWorldAndArgs BindExplicitCommand(
		TEXT("RouteAssignment.BindExplicit"),
		TEXT("Bind one exact DroneID to one zero-based RouteSlot, freeze AllowDegraded, and sync to Console."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&BindExplicit));
}
#endif

