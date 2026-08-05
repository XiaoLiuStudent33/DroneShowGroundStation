#pragma once

#include "CoreMinimal.h"
#include "DroneData.h"
#include "GCSDataTypes.generated.h"

namespace GCSDataSchema
{
	inline constexpr int32 CurrentVersion = 1;
}

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FGCSMessageHeader
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	int32 SchemaVersion = GCSDataSchema::CurrentVersion;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	FString SourceID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	FString SessionID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	int64 Sequence = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	FDateTime SourceTimestamp;
};

UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EGCSDroneDataField : uint8
{
	None = 0 UMETA(Hidden),
	SysID = 1 << 0,
	GPS = 1 << 1,
	Altitude = 1 << 2,
	Battery = 1 << 3,
	Mode = 1 << 4,
	RSSI = 1 << 5,
	Online = 1 << 6,
	LEDColor = 1 << 7,
	All = 0xff UMETA(Hidden)
};
ENUM_CLASS_FLAGS(EGCSDroneDataField);

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FDroneDataDelta
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	FString DroneID;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Drone GCS|Data",
		meta = (Bitmask, BitmaskEnum = "/Script/DroneShowPlatform1.EGCSDroneDataField"))
	int32 ChangedFields = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	FDroneData Data;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	bool bCreateIfMissing = false;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FGCSFleetSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	FGCSMessageHeader Header;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	int64 FleetRevision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	TArray<FDroneData> Drones;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FGCSTelemetryBatch
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	FGCSMessageHeader Header;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	int64 FleetRevision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Data")
	TArray<FDroneDataDelta> Deltas;
};

namespace GCSDataDelta
{
	inline bool HasField(const FDroneDataDelta& Delta, const EGCSDroneDataField Field)
	{
		return EnumHasAnyFlags(
			static_cast<EGCSDroneDataField>(Delta.ChangedFields),
			Field);
	}

	inline void ApplyTo(FDroneData& Target, const FDroneDataDelta& Delta)
	{
		if (HasField(Delta, EGCSDroneDataField::SysID)) Target.SysID = Delta.Data.SysID;
		if (HasField(Delta, EGCSDroneDataField::GPS))
		{
			Target.GPSLat = Delta.Data.GPSLat;
			Target.GPSLon = Delta.Data.GPSLon;
		}
		if (HasField(Delta, EGCSDroneDataField::Altitude)) Target.Altitude = Delta.Data.Altitude;
		if (HasField(Delta, EGCSDroneDataField::Battery)) Target.Battery = Delta.Data.Battery;
		if (HasField(Delta, EGCSDroneDataField::Mode)) Target.Mode = Delta.Data.Mode;
		if (HasField(Delta, EGCSDroneDataField::RSSI)) Target.RSSI = Delta.Data.RSSI;
		if (HasField(Delta, EGCSDroneDataField::Online)) Target.bOnline = Delta.Data.bOnline;
		if (HasField(Delta, EGCSDroneDataField::LEDColor)) Target.LEDColor = Delta.Data.LEDColor;
	}

	inline void Merge(FDroneDataDelta& Target, const FDroneDataDelta& Source)
	{
		ApplyTo(Target.Data, Source);
		Target.ChangedFields |= Source.ChangedFields;
		Target.bCreateIfMissing = Target.bCreateIfMissing || Source.bCreateIfMissing;
	}
}
