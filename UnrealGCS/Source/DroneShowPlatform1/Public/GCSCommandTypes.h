#pragma once

#include "CoreMinimal.h"
#include "GCSCommandTypes.generated.h"

UENUM(BlueprintType)
enum class EGCSCommandResult : uint8
{
	Pong,
	Acknowledged,
	Error
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FGCSCommandResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	FString RequestId;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	FString Command;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	EGCSCommandResult Result = EGCSCommandResult::Error;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	FString Detail;
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FGCSDroneTelemetry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	bool bOnline = false;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	int32 SystemId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	int32 ComponentId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	bool bArmed = false;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	FString Mode = TEXT("UNKNOWN");

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	int32 GpsSatellites = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	double Latitude = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	double Longitude = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	float AltitudeMeters = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	float RelativeAltitudeMeters = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	float GroundSpeedMetersPerSecond = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	int32 BatteryPercent = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	float RollRadians = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	float PitchRadians = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS")
	float YawRadians = 0.0f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FGCSCommandResponseEvent,
	const FGCSCommandResponse&,
	Response);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FGCSTelemetryEvent,
	const FGCSDroneTelemetry&,
	Telemetry);
