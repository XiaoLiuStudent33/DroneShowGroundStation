/**
 * @brief 定义当前 Production UI 与 DSP Command Provider 使用的 UE 领域命令类型。
 *
 * 分类：Production。
 * HoverTest 是应用级工作流；SetMode 通过 MAVLink #11，不应误记为 MAV_CMD。
 */
#pragma once

#include "CoreMinimal.h"
#include "DSPGCSProductionCommandTypes.generated.h"

UENUM(BlueprintType)
enum class EGCSProductionCommandType : uint8
{
	Arm,
	Disarm,
	Takeoff,
	Land,
	RTL,
	SetMode,
	HoverTest,
	SetLedColor,
	TimeStart,
	TimeStop,
	ExecuteDanceStep,
	EndDanceStep,
	MissionPause,
	MissionForward,
	MissionBackward,
	ChangeSpeed,
	TriggerSmoke,
	StopTelemetry,
	ResumeTelemetry,
	DisableLedOutput,
	RestoreLedOutput,
	Wake,
	Standby,
	Reboot,
	Shutdown
};

/** Ground-station workflow mode; intentionally independent of APM flight mode. */
UENUM(BlueprintType)
enum class EGCSGroundStationControlMode : uint8
{
	Manual,
	Autonomous
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FGCSProductionCommandParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	float TakeoffAltitudeMeters = 0.0f;

	/** Target altitude used by the Phase 5.3 HoverTest state machine. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	float HoverAltitudeMeters = 0.0f;

	/** Retained for protocol compatibility; Phase 5.3 does not use a duration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	float HoverDurationSeconds = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	int32 CustomMode = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	FString ModeName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	bool bStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	uint8 Red = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	uint8 Green = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	uint8 Blue = 0;

	/** MP_TIME_CONTROL action mask. Flight scheduling uses mask 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	uint8 TimeControlMask = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	uint8 ScheduledHour = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	uint8 ScheduledMinute = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	uint8 ScheduledSecond = 0;

	/** Audited legacy speed range is 1-5 m/s; Console validates again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone GCS|Command")
	float AdvancedSpeedMetersPerSecond = 0.0f;
};

/** Application result semantics; values are independent of wire ACK numbers. */
UENUM(BlueprintType)
enum class EGCSCommandResultState : uint8
{
	Unknown,
	Received,
	Validated,
	Queued,
	Sent,
	InProgress,
	Accepted,
	Rejected,
	Failed,
	TimedOut,
	Cancelled,
	Simulated
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FGCSProductionCommandResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	FString CommandID;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	FString DroneID;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	EGCSProductionCommandType CommandType = EGCSProductionCommandType::Arm;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	FString Source;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	FString AckState;

	/** Stable decision input. AckState remains unchanged display text only. */
	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	EGCSCommandResultState State = EGCSCommandResultState::Unknown;

	/** Preserves the existing Sent/Accepted/Simulated UI policy, not physical completion. */
	bool IsSuccessfulDispatch() const
	{
		return State == EGCSCommandResultState::Sent
			|| State == EGCSCommandResultState::Accepted
			|| State == EGCSCommandResultState::Simulated;
	}

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	FString ErrorCode;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	FString Detail;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	bool bSimulated = false;

	UPROPERTY(BlueprintReadOnly, Category = "Drone GCS|Command")
	bool bFinal = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FGCSProductionCommandResultEvent,
	const FGCSProductionCommandResult&,
	Result);
