/**
 * @brief 定义 UE Fleet/UI/Visualization 使用的单架无人机领域快照。
 *
 * 分类：Production。
 * 输入：协议 Provider 转换后的字段；输出：FleetManager、UI Bridge、Cesium/Niagara。
 * 关键约束：该类型不暴露 DSP-GCS 或 MAVLink Wire Format 细节。
 */
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

	/** Stable Fleet transport identity supplied by Console (Remote IP + UDP source port). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone")
	FString EndpointIdentity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|GPS")
	double GPSLat = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|GPS")
	double GPSLon = 0.0;

	/** Provider-reported GPS validity; use DroneGPS::IsValid for fleet decisions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|GPS")
	bool bGPSValid = false;

	/** MAVLink-compatible fix type. A 3D fix (3) is the current minimum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|GPS")
	int32 GPSFixType = 0;

	/** Console receive time of the GPS sample, in Unix UTC milliseconds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone|GPS")
	int64 GPSSampleTimestampUtcMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Flight")
	float Altitude = 0.0f;

	/** Absolute altitude above mean sea level; never substitutes Altitude. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Flight")
	float AltitudeAMSL = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Flight")
	bool bAltitudeValid = false;

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

	/** True only when LEDColor came from confirmed aircraft feedback. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone|Show")
	bool bLEDColorValid = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Selection")
	bool bSelected = false;
};

namespace DroneGPS
{
	inline constexpr int32 MinimumValidFixType = 3;

	inline bool IsValid(const FDroneData& DroneData)
	{
		return DroneData.bGPSValid
			&& DroneData.GPSFixType >= MinimumValidFixType
			&& !(DroneData.GPSLat == 0.0 && DroneData.GPSLon == 0.0);
	}
}
