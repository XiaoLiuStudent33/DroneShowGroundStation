#pragma once

#include "CoreMinimal.h"
#include "RouteArtifactManifest.h"
#include "RouteWaypointValidator.h"

enum class ERouteArtifactCurrentStatus : uint8
{
	None,
	Ready,
	Invalid
};

enum class ERouteArtifactGenerationStatus : uint8
{
	None,
	Creating,
	Generating,
	Validating,
	Finalizing,
	Failed
};

enum class ERouteArtifactRecoveryStatus : uint8
{
	Stable,
	Recovered,
	RecoveryRequired,
	Unrecoverable,
	Busy
};

enum class ERouteArtifactRecoveryErrorCode : uint8
{
	None,
	OrphanStagingCleaned,
	RecoveredPreviousCurrent,
	InvalidCurrent,
	MultiplePreviousCandidates,
	ReparsePointDetected,
	UnexpectedWorkspaceEntry,
	RollbackRequired,
	FilesystemOperationFailed,
	WriterActive
};

struct DRONESHOWPLATFORM1_API FRouteArtifactRecoveryResult
{
	ERouteArtifactRecoveryStatus Status = ERouteArtifactRecoveryStatus::Stable;
	ERouteArtifactRecoveryErrorCode ErrorCode = ERouteArtifactRecoveryErrorCode::None;
	FString RecoverySummary;
	int32 OrphanAttemptsCleaned = 0;
	int32 PreviousDirectoriesCleaned = 0;
};

/** Immutable Authority observations supplied to the filesystem/domain manager. */
struct DRONESHOWPLATFORM1_API FRouteArtifactWorkspaceAuthorityGate
{
	bool bHasFrozenSnapshot = false;
	FString FrozenFleetSessionID;
	FString FrozenAssignmentAuthoritySessionID;
	int64 FrozenAssignmentRevision = 0;

	bool bConsoleAccepted = false;
	FRouteArtifactAssignmentBinding ConsoleAcceptedBinding;

	bool bSourceExists = false;
	FRouteArtifactSourceIdentity ActiveSourceIdentity;
	int32 RouteSlotCount = 0;
};

struct DRONESHOWPLATFORM1_API FRouteArtifactGenerationHandle
{
	FString AttemptID;
	FString ArtifactSetID;
	FString StagingRoot;
};

struct DRONESHOWPLATFORM1_API FRouteArtifactCurrentInfo
{
	FString ArtifactSetID;
	FRouteArtifactDigest256 ArtifactSetDigest;
	FRouteArtifactAssignmentBinding AssignmentBinding;
	FRouteArtifactDigest256 SourceDatasetDigest;
	int32 RouteSlotCount = 0;
	int32 MatchedCount = 0;
	FString CurrentRoot;
	int64 CreatedAtUtc = 0;
};

/** Coarse production-stage timings; no per-route or per-frame instrumentation. */
struct DRONESHOWPLATFORM1_API FRouteArtifactFinalizeMetrics
{
	double AuthoritativeRouteValidationSeconds = 0.0;
	double ManifestConstructionSerializationSeconds = 0.0;
	double ReadyPromotionSeconds = 0.0;
};

/**
 * Pure filesystem/domain owner for one Route Artifact Workspace root.
 * Authority capture and GameInstance lifecycle remain in the subsystem.
 */
class DRONESHOWPLATFORM1_API FRouteArtifactWorkspaceManager final
{
public:
	FRouteArtifactWorkspaceManager();
	~FRouteArtifactWorkspaceManager();

	FRouteArtifactWorkspaceManager(const FRouteArtifactWorkspaceManager&) = delete;
	FRouteArtifactWorkspaceManager& operator=(const FRouteArtifactWorkspaceManager&) = delete;

	bool SetConfiguredWorkspaceRoot(const FString& ConfiguredRoot, FString& OutError);
	const FString& GetConfiguredWorkspaceRoot() const;
	bool ResolveWorkspaceRoot(FString& OutRoot, FString& OutError) const;
	bool GetWorkspaceVolumeFreeBytes(int64& OutFreeBytes, FString& OutError) const;

	ERouteArtifactCurrentStatus GetCurrentStatus() const;
	ERouteArtifactGenerationStatus GetGenerationStatus() const;
	bool IsCurrentSwapInProgress() const;
	bool HasActiveGeneration() const;
	const FRouteArtifactCurrentInfo& GetCurrentInfo() const;
	const FRouteArtifactRecoveryResult& GetLastRecoveryResult() const;

	/** Acquires the same per-root writer authority used by generation. */
	bool RecoverWorkspace(FRouteArtifactRecoveryResult& OutResult, FString& OutError);

	bool BeginGeneration(
		const FRouteArtifactBuildIdentity& BuildIdentity,
		const FRouteArtifactWorkspaceAuthorityGate& Gate,
		FRouteArtifactGenerationHandle& OutHandle,
		FString& OutError);

	/** The only production route-output resolver: RouteSlotIndex to safe staging path. */
	bool ResolveSafeStagingRoutePath(
		int32 RouteSlotIndex,
		FString& OutPath,
		FString& OutError) const;

	bool FinalizeGeneration(
		const FRouteArtifactManifest& Manifest,
		const FRouteArtifactWorkspaceAuthorityGate& CurrentGate,
		FString& OutError);

	/**
	 * Production generator finalize: hashes each closed physical staging route
	 * and validates it against its captured source in one physical read, then
	 * fills canonical route records/digest and promotes Current.
	 */
	bool FinalizeGeneratedRoutes(
		FRouteArtifactManifest& InOutManifest,
		const FRouteArtifactWorkspaceAuthorityGate& CurrentGate,
		const FRouteWaypointSourceValidationContext& SourceValidationContext,
		FString& OutError,
		FRouteArtifactFinalizeMetrics* OutMetrics = nullptr,
		const TFunction<bool()>& IsCancelled = TFunction<bool()>());

	/** Returns false with GenerationNotActive when repeated. */
	bool AbortGeneration(FString& OutError);

	bool LoadAndValidateCurrentArtifact(
		FRouteArtifactManifest& OutManifest,
		FString& OutError);

	/** Releases writer ownership; an undeleted attempt is handled by recovery. */
	void Shutdown();

	static bool SerializeReadyMarker(
		const FString& ArtifactSetID,
		const FRouteArtifactDigest256& ArtifactSetDigest,
		TArray<uint8>& OutBytes,
		FString& OutError);
	static bool ParseReadyMarker(
		const TArray<uint8>& Bytes,
		FString& OutArtifactSetID,
		FRouteArtifactDigest256& OutArtifactSetDigest,
		FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
	/** Fails only the attempt-to-Current directory move, after Current-to-previous. */
	void SetTestFailAttemptPromotion(bool bFail);
	int64 GetTestFinalizeRouteValidationReadCount() const;
	static bool ValidateCurrentEntryAllowlistForTest(
		const FRouteArtifactManifest& Manifest,
		const TArray<FString>& RelativeFiles,
		const TArray<FString>& RelativeDirectories,
		FString& OutError);
	static bool TryAcquireCurrentPromotionGateForTest(
		const FString& WorkspaceRoot,
		bool& bOutRecoveredAbandonedOwner,
		FString& OutError);
#endif

private:
	bool FinalizeGenerationInternal(
		const FRouteArtifactManifest& Manifest,
		const FRouteArtifactWorkspaceAuthorityGate& CurrentGate,
		bool bRouteFilesAlreadyAuthoritativelyHashed,
		FString& OutError,
		FRouteArtifactFinalizeMetrics* OutMetrics = nullptr);
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
