#pragma once

#include "CoreMinimal.h"
#include "DroneData.generated.h"

/**
 * UI-facing snapshot of one drone.
 *
 * This type deliberately contains no transport or MAVLink details. Future
 * communication adapters translate their protocol data into this structure
 * before publishing it to the fleet layer.
 */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FDroneData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone")
	FString DroneID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone")
	int32 SysID = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|GPS")
	double GPSLat = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|GPS")
	double GPSLon = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Flight")
	float Altitude = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Status", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float Battery = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Flight")
	FString Mode = TEXT("UNKNOWN");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Status")
	int32 RSSI = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Status")
	bool bOnline = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Show")
	FLinearColor LEDColor = FLinearColor::Black;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Selection")
	bool bSelected = false;
};
