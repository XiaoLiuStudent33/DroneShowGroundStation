#pragma once

#include "CoreMinimal.h"
#include "RouteAssignmentTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RouteAssignmentSubsystem.generated.h"

/** GameInstance authority for working and frozen DroneID to RouteSlot assignments. */
UCLASS()
class DRONESHOWPLATFORM1_API URouteAssignmentSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	ERouteAssignmentState GetState() const { return State; }
	ERouteAssignmentReadinessKind GetReadinessKind() const { return WorkingSnapshot.ReadinessKind; }
	const FString& GetAssignmentAuthoritySessionID() const { return AssignmentAuthoritySessionID; }
	int64 GetCurrentAssignmentRevision() const { return CurrentAssignmentRevision; }
	bool IsFrozen() const { return State == ERouteAssignmentState::Frozen; }

	/** Copies the current Fleet and active Route Frame-0 sources without changing assignment authority state. */
	bool CaptureSourceSnapshot(FRouteAssignmentSourceSnapshot& OutSnapshot, FString& OutError) const;

	/** Validates and atomically replaces the working assignment. Rejected candidates do not issue a revision. */
	bool ReplaceWorkingAssignment(const FRouteAssignmentCandidate& Candidate, FString& OutError);

	/** Captures current Fleet/Route sources, builds a production candidate, and atomically replaces Working. */
	bool BuildAndReplaceWorkingAssignment(FString& OutError);

	/** Returns whether the current working assignment can be frozen, with no state mutation. */
	bool CanFreeze(ERouteAssignmentFreezePolicy FreezePolicy, FString& OutError) const;

	/** Copies the Ready working snapshot into immutable authority-owned frozen storage. */
	bool Freeze(ERouteAssignmentFreezePolicy FreezePolicy, FString& OutError);

	/** Returns the authority-owned working snapshot by value through OutSnapshot. */
	bool GetWorkingSnapshot(FRouteAssignmentSnapshot& OutSnapshot) const;

	/** Returns the authority-owned frozen snapshot by value through OutSnapshot. */
	bool GetFrozenSnapshot(FRouteAssignmentSnapshot& OutSnapshot) const;

	/** Clears current state while retaining LastIssuedAssignmentRevision. */
	void Reset();

private:
	FString AssignmentAuthoritySessionID;
	ERouteAssignmentState State = ERouteAssignmentState::Empty;
	int64 LastIssuedAssignmentRevision = 0;
	int64 CurrentAssignmentRevision = 0;
	FRouteAssignmentSnapshot WorkingSnapshot;
	FRouteAssignmentSnapshot FrozenSnapshot;

	static bool ValidateCandidate(const FRouteAssignmentCandidate& Candidate, FString& OutError);
	static void CanonicalizeCandidate(FRouteAssignmentCandidate& Candidate);
	static ERouteAssignmentReadinessKind DetermineReadinessKind(const FRouteAssignmentCandidate& Candidate);
	static FRouteAssignmentSummary BuildSummary(const FRouteAssignmentCandidate& Candidate);
	bool ValidateWorkingSnapshotForFreeze(
		ERouteAssignmentFreezePolicy FreezePolicy,
		FString& OutError) const;
};
