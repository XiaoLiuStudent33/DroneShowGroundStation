#pragma once

#include "CoreMinimal.h"
#include "RouteAssignmentTypes.generated.h"

UENUM(BlueprintType)
enum class ERouteAssignmentState : uint8
{
	Empty,
	Draft,
	Ready,
	Frozen
};

UENUM(BlueprintType)
enum class ERouteAssignmentReadinessKind : uint8
{
	NotReady,
	FullReady,
	DegradedReady
};

UENUM(BlueprintType)
enum class ERouteAssignmentFreezePolicy : uint8
{
	Unset,
	RequireFull,
	AllowDegraded
};

UENUM(BlueprintType)
enum class ERouteAssignmentCompletionMode : uint8
{
	Unset,
	Full,
	Degraded
};

UENUM(BlueprintType)
enum class ERouteAssignmentExcludedReason : uint8
{
	Unknown,
	Offline,
	InvalidGeo,
	InvalidGPS,
	InsufficientGPSFix
};

UENUM(BlueprintType)
enum class ERouteAssignmentUnresolvedReason : uint8
{
	Unknown,
	Ambiguous,
	NoCandidate,
	Conflict
};

UENUM(BlueprintType)
enum class ERouteAssignmentConflictType : uint8
{
	Unknown,
	MultipleDronesForRouteSlot,
	DroneAssignedToMultipleRouteSlots,
	DuplicateDrone,
	DuplicateRouteSlot
};

/** A resolved, legal DroneID to zero-based RouteSlotIndex mapping. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentEntry
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotIndex = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double HorizontalDistanceMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	bool bHorizontalDistanceValid = false;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentSpareDrone
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentExcludedDrone
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	ERouteAssignmentExcludedReason Reason = ERouteAssignmentExcludedReason::Unknown;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentUnresolvedDrone
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	ERouteAssignmentUnresolvedReason Reason = ERouteAssignmentUnresolvedReason::Unknown;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentConflict
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	ERouteAssignmentConflictType ConflictType = ERouteAssignmentConflictType::Unknown;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FString> DroneIDs;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<int32> RouteSlotIndices;
};

/** Source authority identities copied by value at assignment capture time. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentSourceIdentity
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString FleetSessionID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int64 FleetRevisionAtCapture = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int64 RouteDatasetGeneration = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	bool bShowOriginSet = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double Latitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double Longitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double Altitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FDateTime CapturedAtUtc;
};

/** Fleet authority values copied for one route-assignment source capture. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentDroneSource
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double Latitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double Longitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	float AltitudeAMSL = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	bool bGPSValid = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 GPSFixType = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	bool bAltitudeValid = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	bool bOnline = false;
};

/** Horizontal Local EN derived from a raw source snapshot for assignment use. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentDroneLocalPosition
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double EastMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double NorthMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	bool bAssignmentGeoEligible = false;
};

/** Frame-0 Local ENU route values copied for one source capture. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentRouteSlotSource
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotIndex = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double EastMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double NorthMeters = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double UpMeters = 0.0;
};

/** Stable value copy of the Fleet and active Route sources at one GameThread instant. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentSourceSnapshot
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FRouteAssignmentSourceIdentity SourceIdentity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentDroneSource> Drones;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentRouteSlotSource> RouteSlots;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentSummary
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 MatchedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 SpareCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 ExcludedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 UnresolvedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 VacantRouteSlotCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 ConflictCount = 0;
};

/** Input value object supplied by a future matcher. State and revision are authority-owned. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentCandidate
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FRouteAssignmentSourceIdentity SourceIdentity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 CandidateDroneCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentEntry> MatchedEntries;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentSpareDrone> SpareDrones;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentExcludedDrone> ExcludedDrones;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentUnresolvedDrone> UnresolvedDrones;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<int32> VacantRouteSlots;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentConflict> Conflicts;
};

/** Authority-owned assignment value object returned to readers by copy. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentSnapshot
{
	GENERATED_BODY()

	/** Opaque epoch shared by every assignment issued by one authority lifetime. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString AssignmentAuthoritySessionID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int64 AssignmentRevision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FRouteAssignmentSourceIdentity SourceIdentity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 CandidateDroneCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentEntry> MatchedEntries;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentSpareDrone> SpareDrones;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentExcludedDrone> ExcludedDrones;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentUnresolvedDrone> UnresolvedDrones;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<int32> VacantRouteSlots;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentConflict> Conflicts;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FRouteAssignmentSummary Summary;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	ERouteAssignmentReadinessKind ReadinessKind = ERouteAssignmentReadinessKind::NotReady;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	ERouteAssignmentCompletionMode CompletionMode = ERouteAssignmentCompletionMode::Unset;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	ERouteAssignmentFreezePolicy FreezePolicy = ERouteAssignmentFreezePolicy::Unset;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FDateTime FrozenAtUtc;
};
