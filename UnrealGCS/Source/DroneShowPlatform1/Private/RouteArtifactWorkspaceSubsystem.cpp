#include "RouteArtifactWorkspaceSubsystem.h"

#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "Engine/GameInstance.h"
#include "RouteAssignmentSubsystem.h"
#include "RoutePreviewSubsystem.h"
#include "RouteWaypointGenerator.h"

DEFINE_LOG_CATEGORY_STATIC(LogRouteArtifactWorkspaceSubsystem, Log, All);

void URouteArtifactWorkspaceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Manager = MakeUnique<FRouteArtifactWorkspaceManager>();
	FRouteArtifactRecoveryResult Ignored;
	FString IgnoredError;
	Manager->RecoverWorkspace(Ignored, IgnoredError);
}

void URouteArtifactWorkspaceSubsystem::Deinitialize()
{
	if (Manager)
	{
		Manager->Shutdown();
		Manager.Reset();
	}
	Super::Deinitialize();
}

bool URouteArtifactWorkspaceSubsystem::SetConfiguredWorkspaceRoot(
	const FString& ConfiguredRoot,
	FString& OutError)
{
	return Manager && Manager->SetConfiguredWorkspaceRoot(ConfiguredRoot, OutError);
}

FString URouteArtifactWorkspaceSubsystem::GetConfiguredWorkspaceRoot() const
{
	return Manager ? Manager->GetConfiguredWorkspaceRoot() : FString{};
}

bool URouteArtifactWorkspaceSubsystem::ResolveWorkspaceRoot(
	FString& OutRoot,
	FString& OutError) const
{
	return Manager && Manager->ResolveWorkspaceRoot(OutRoot, OutError);
}

bool URouteArtifactWorkspaceSubsystem::GetWorkspaceVolumeFreeBytes(
	int64& OutFreeBytes,
	FString& OutError) const
{
	return Manager && Manager->GetWorkspaceVolumeFreeBytes(OutFreeBytes, OutError);
}

bool URouteArtifactWorkspaceSubsystem::RecoverWorkspace(
	FRouteArtifactRecoveryResult& OutResult,
	FString& OutError)
{
	check(IsInGameThread());
	return Manager && Manager->RecoverWorkspace(OutResult, OutError);
}

bool URouteArtifactWorkspaceSubsystem::BeginGeneration(
	const FRouteArtifactBuildIdentity& BuildIdentity,
	FRouteArtifactGenerationHandle& OutHandle,
	FString& OutError)
{
	check(IsInGameThread());
	FRouteArtifactWorkspaceAuthorityGate Gate;
	return Manager && CaptureAuthorityGate(Gate, OutError)
		&& Manager->BeginGeneration(BuildIdentity, Gate, OutHandle, OutError);
}

bool URouteArtifactWorkspaceSubsystem::BeginGeneration(
	const FRouteArtifactBuildIdentityV2& BuildIdentity,
	const FRouteArtifactWorkspaceSourceGate& CapturedSourceGate,
	FRouteArtifactGenerationHandle& OutHandle,
	FString& OutError)
{
	check(IsInGameThread());
	return Manager && Manager->BeginGeneration(
		BuildIdentity, CapturedSourceGate, OutHandle, OutError);
}

bool URouteArtifactWorkspaceSubsystem::ResolveSafeStagingRoutePath(
	const int32 RouteSlotIndex,
	FString& OutPath,
	FString& OutError) const
{
	return Manager && Manager->ResolveSafeStagingRoutePath(RouteSlotIndex, OutPath, OutError);
}

bool URouteArtifactWorkspaceSubsystem::FinalizeGeneration(
	const FRouteArtifactManifest& Manifest,
	FString& OutError)
{
	check(IsInGameThread());
	FRouteArtifactWorkspaceAuthorityGate Gate;
	if (!Manager || !CaptureAuthorityGate(Gate, OutError))
	{
		// A failed recapture is itself a Finalize gate failure. Abort releases ownership.
		if (Manager && Manager->HasActiveGeneration())
		{
			FString AbortError;
			Manager->AbortGeneration(AbortError);
		}
		return false;
	}
	return Manager->FinalizeGeneration(Manifest, Gate, OutError);
}

bool URouteArtifactWorkspaceSubsystem::FinalizeGeneration(
	const FRouteArtifactManifestV2& Manifest,
	FString& OutError)
{
	check(IsInGameThread());
	FRouteArtifactWorkspaceSourceGate Gate;
	if (!Manager || !CaptureSourceGate(Gate, OutError))
	{
		if (Manager && Manager->HasActiveGeneration())
		{
			FString AbortError;
			Manager->AbortGeneration(AbortError);
		}
		return false;
	}
	return Manager->FinalizeGeneration(Manifest, Gate, OutError);
}

bool URouteArtifactWorkspaceSubsystem::FinalizeGeneratedRoutes(
	FRouteArtifactManifest& InOutManifest,
	const FRouteWaypointSourceValidationContext& SourceValidationContext,
	FString& OutError)
{
	check(IsInGameThread());
	FRouteArtifactWorkspaceAuthorityGate Gate;
	if (!Manager || !CaptureAuthorityGate(Gate, OutError))
	{
		if (Manager && Manager->HasActiveGeneration())
		{
			FString AbortError;
			Manager->AbortGeneration(AbortError);
		}
		return false;
	}
	return Manager->FinalizeGeneratedRoutes(
		InOutManifest, Gate, SourceValidationContext, OutError);
}

bool URouteArtifactWorkspaceSubsystem::FinalizeGeneratedRoutes(
	FRouteArtifactManifestV2& InOutManifest,
	const FRouteWaypointSourceValidationContext& SourceValidationContext,
	FString& OutError)
{
	check(IsInGameThread());
	FRouteArtifactWorkspaceSourceGate Gate;
	if (!Manager || !CaptureSourceGate(Gate, OutError))
	{
		if (Manager && Manager->HasActiveGeneration())
		{
			FString AbortError;
			Manager->AbortGeneration(AbortError);
		}
		return false;
	}
	return Manager->FinalizeGeneratedRoutes(
		InOutManifest, Gate, SourceValidationContext, OutError);
}

bool URouteArtifactWorkspaceSubsystem::AbortGeneration(FString& OutError)
{
	check(IsInGameThread());
	return Manager && Manager->AbortGeneration(OutError);
}

bool URouteArtifactWorkspaceSubsystem::LoadAndValidateCurrentArtifact(
	FRouteArtifactManifest& OutManifest,
	FString& OutError)
{
	return Manager && Manager->LoadAndValidateCurrentArtifact(OutManifest, OutError);
}

bool URouteArtifactWorkspaceSubsystem::LoadAndValidateCurrentArtifact(
	FRouteArtifactManifestV2& OutManifest,
	FString& OutError)
{
	return Manager && Manager->LoadAndValidateCurrentArtifact(OutManifest, OutError);
}

ERouteArtifactCurrentStatus URouteArtifactWorkspaceSubsystem::GetCurrentStatus() const
{
	return Manager ? Manager->GetCurrentStatus() : ERouteArtifactCurrentStatus::None;
}

ERouteArtifactGenerationStatus URouteArtifactWorkspaceSubsystem::GetGenerationStatus() const
{
	return Manager ? Manager->GetGenerationStatus() : ERouteArtifactGenerationStatus::None;
}

bool URouteArtifactWorkspaceSubsystem::IsCurrentSwapInProgress() const
{
	return Manager && Manager->IsCurrentSwapInProgress();
}

const FRouteArtifactCurrentInfo& URouteArtifactWorkspaceSubsystem::GetCurrentInfo() const
{
	static const FRouteArtifactCurrentInfo Empty;
	return Manager ? Manager->GetCurrentInfo() : Empty;
}

const FRouteArtifactRecoveryResult&
URouteArtifactWorkspaceSubsystem::GetLastRecoveryResult() const
{
	static const FRouteArtifactRecoveryResult Empty;
	return Manager ? Manager->GetLastRecoveryResult() : Empty;
}

const FRouteArtifactSourceIdentity*
URouteArtifactWorkspaceSubsystem::GetCurrentSourceIdentity() const
{
	return Manager ? Manager->GetCurrentSourceIdentity() : nullptr;
}

bool URouteArtifactWorkspaceSubsystem::FindCurrentRouteRecord(
	const int32 RouteSlotIndex,
	FRouteArtifactRecord& OutRecord) const
{
	return Manager && Manager->FindCurrentRouteRecord(RouteSlotIndex, OutRecord);
}

bool URouteArtifactWorkspaceSubsystem::CaptureAuthorityGate(
	FRouteArtifactWorkspaceAuthorityGate& OutGate,
	FString& OutError) const
{
	OutGate = FRouteArtifactWorkspaceAuthorityGate{};
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		OutError = TEXT("Artifact Authority gate has no GameInstance.");
		return false;
	}
	URouteAssignmentSubsystem* Assignment =
		GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
		GameInstance->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
	URoutePreviewSubsystem* RoutePreview =
		GameInstance->GetSubsystem<URoutePreviewSubsystem>();
	if (!Assignment || !Sync || !RoutePreview)
	{
		OutError = TEXT("Artifact Authority gate subsystem is unavailable.");
		return false;
	}

	FRouteAssignmentSnapshot Frozen;
	OutGate.bHasFrozenSnapshot = Assignment->GetFrozenSnapshot(Frozen);
	if (OutGate.bHasFrozenSnapshot)
	{
		OutGate.FrozenFleetSessionID = Frozen.SourceIdentity.FleetSessionID;
		OutGate.FrozenAssignmentAuthoritySessionID = Frozen.AssignmentAuthoritySessionID;
		OutGate.FrozenAssignmentRevision = Frozen.AssignmentRevision;
	}
	const FDSPGCSFrozenAssignmentSyncStatus Status = Sync->GetSyncStatus();
	OutGate.bConsoleAccepted =
		Status.State == EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted;
	OutGate.ConsoleAcceptedBinding.FleetSessionID = Status.FleetSessionID;
	OutGate.ConsoleAcceptedBinding.AssignmentAuthoritySessionID =
		Status.AssignmentAuthoritySessionID;
	OutGate.ConsoleAcceptedBinding.AssignmentRevision = Status.AssignmentRevision;
	if (!FRouteArtifactDigest256::ParseLowerHex(
		Status.CanonicalPayloadDigest,
		OutGate.ConsoleAcceptedBinding.AssignmentDigest,
		OutError))
	{
		return false;
	}

	OutGate.bSourceExists = RoutePreview->IsRouteLoaded();
	OutGate.ActiveSourceIdentity = RoutePreview->GetActiveSourceIdentity();
	OutGate.RouteSlotCount = RoutePreview->GetDroneCount();
	return true;
}

bool URouteArtifactWorkspaceSubsystem::CaptureSourceGate(
	FRouteArtifactWorkspaceSourceGate& OutGate,
	FString& OutError) const
{
	check(IsInGameThread());
	OutGate = FRouteArtifactWorkspaceSourceGate{};
	UGameInstance* GameInstance = GetGameInstance();
	URoutePreviewSubsystem* RoutePreview = GameInstance
		? GameInstance->GetSubsystem<URoutePreviewSubsystem>() : nullptr;
	if (!RoutePreview)
	{
		OutError = TEXT("Route source gate subsystem is unavailable.");
		return false;
	}
	const TSharedPtr<const FRoutePreviewData, ESPMode::ThreadSafe> RouteData =
		RoutePreview->CaptureRouteDataSnapshot();
	if (!RouteData.IsValid() || !RouteData->IsLoaded() || !RouteData->bShowOriginSet)
	{
		OutError = TEXT("A loaded Route dataset with Show Origin is required.");
		return false;
	}
	FRouteWaypointGeneratorContext GeneratorContext;
	FRouteWaypointGeneratorError GeneratorError;
	if (!RouteWaypointGenerator::BuildContext(
		RouteData->ShowOriginLatitude,
		RouteData->ShowOriginLongitude,
		GeneratorContext,
		GeneratorError))
	{
		OutError = GeneratorError.Diagnostic;
		return false;
	}
	OutGate.bSourceExists = true;
	OutGate.ActiveSourceIdentity = RouteData->SourceIdentity;
	OutGate.RouteDatasetGeneration = RoutePreview->GetRouteDatasetGeneration();
	OutGate.FrameCount = RouteData->FrameCount;
	OutGate.RouteSlotCount = RouteData->DroneCount;
	OutGate.bHasRange = RouteData->OptionalRangeStart != INDEX_NONE
		&& RouteData->OptionalRangeEnd != INDEX_NONE;
	OutGate.RangeStart = OutGate.bHasRange ? RouteData->OptionalRangeStart : 0;
	OutGate.RangeEnd = OutGate.bHasRange ? RouteData->OptionalRangeEnd : 0;
	OutGate.ShowOriginLatitudeE7 = GeneratorContext.NormalizedShowOriginLatitudeE7;
	OutGate.ShowOriginLongitudeE7 = GeneratorContext.NormalizedShowOriginLongitudeE7;
	OutGate.WaypointGeneratorVersion = GeneratorContext.WaypointGeneratorVersion;
	OutGate.GeneratorInputDigest = GeneratorContext.GeneratorInputDigest;
	OutError.Reset();
	return true;
}
