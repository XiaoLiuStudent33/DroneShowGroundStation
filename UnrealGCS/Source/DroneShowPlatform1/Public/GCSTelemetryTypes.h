#pragma once

#include "CoreMinimal.h"
#include "GCSTelemetryTypes.generated.h"

// Existing reflected contracts, independent of the transport implementation.
UENUM(BlueprintType)
enum class EDSPGCSTelemetryLinkState : uint8
{
	Disconnected,
	Connected
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDSPGCSTelemetryLinkStateChangedEvent);

USTRUCT(BlueprintType)
struct FMDHKTelemetryTableRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FString DroneID;
	UPROPERTY(BlueprintReadOnly) bool bReceived = false;
	UPROPERTY(BlueprintReadOnly) int64 SampleTimestampUtcMs = 0;
	UPROPERTY(BlueprintReadOnly) int32 BatteryRemainingPercent = -1;
	UPROPERTY(BlueprintReadOnly) bool bBatteryRemainingValid = false;
	UPROPERTY(BlueprintReadOnly) double BatteryTemperatureCelsius = 0.0;
	UPROPERTY(BlueprintReadOnly) bool bBatteryTemperatureValid = false;
	UPROPERTY(BlueprintReadOnly) double Cell1Volts = 0.0;
	UPROPERTY(BlueprintReadOnly) bool bCell1Valid = false;
	UPROPERTY(BlueprintReadOnly) double Cell2Volts = 0.0;
	UPROPERTY(BlueprintReadOnly) bool bCell2Valid = false;
	UPROPERTY(BlueprintReadOnly) double Cell3Volts = 0.0;
	UPROPERTY(BlueprintReadOnly) bool bCell3Valid = false;
	UPROPERTY(BlueprintReadOnly) double Cell4Volts = 0.0;
	UPROPERTY(BlueprintReadOnly) bool bCell4Valid = false;
	UPROPERTY(BlueprintReadOnly) double LatitudeDeg = 0.0;
	UPROPERTY(BlueprintReadOnly) double LongitudeDeg = 0.0;
	UPROPERTY(BlueprintReadOnly) bool bGPSValid = false;
	UPROPERTY(BlueprintReadOnly) double AltitudeAmslMeters = 0.0;
	UPROPERTY(BlueprintReadOnly) bool bAltitudeValid = false;
	UPROPERTY(BlueprintReadOnly) int64 ValidFlags = 0;
	UPROPERTY(BlueprintReadOnly) int32 PayloadSchemaVersion = 0;
	UPROPERTY(BlueprintReadOnly) int64 TimeBootMs = 0;
	UPROPERTY(BlueprintReadOnly) FString GPSUtc;
	UPROPERTY(BlueprintReadOnly) FString ValidFlagsText;
	UPROPERTY(BlueprintReadOnly) double RelativeAltitudeMeters = 0.0;
	UPROPERTY(BlueprintReadOnly) int64 FreeMemoryBytes = 0;
	UPROPERTY(BlueprintReadOnly) FString FlightSoftwareVersionText;
	UPROPERTY(BlueprintReadOnly) double WaypointDistanceMeters = 0.0;
	UPROPERTY(BlueprintReadOnly) double CompassVariance = 0.0;
	UPROPERTY(BlueprintReadOnly) double MagneticField1Mgauss = 0.0;
	UPROPERTY(BlueprintReadOnly) double MagneticField2Mgauss = 0.0;
	UPROPERTY(BlueprintReadOnly) int32 BatteryCycleCount = -1;
	UPROPERTY(BlueprintReadOnly) int32 CellVoltageDeltaMv = -1;
	UPROPERTY(BlueprintReadOnly) FString EKFFlagsText;
	UPROPERTY(BlueprintReadOnly) int32 MissionSequence = 0;
	UPROPERTY(BlueprintReadOnly) int32 MissionTotal = 0;
	UPROPERTY(BlueprintReadOnly) double BatteryCurrentAmps = -1.0;
	UPROPERTY(BlueprintReadOnly) int32 GPSFixType = 0;
	UPROPERTY(BlueprintReadOnly) int32 SatellitesVisible = -1;
	UPROPERTY(BlueprintReadOnly) FString RGBText;
	UPROPERTY(BlueprintReadOnly) FString FlightCustomVersionText;
	UPROPERTY(BlueprintReadOnly) FString HardwareID;
};

