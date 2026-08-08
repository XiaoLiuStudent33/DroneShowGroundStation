/**
 * @brief UE 当前 Production 命令 Provider。
 *
 * 分类：Production。
 * 输入：UI 领域命令和稳定 DroneID；输出：DSP-GCS CommandRequest，并接收 CommandResult。
 * 调用链：GCSUIBridge -> ProductionCommandProvider -> ProductionTelemetry Transport -> Console。
 */
#pragma once

#include "CoreMinimal.h"
#include "DSPGCSProtocol.h"
#include "DSPGCSProductionCommandTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "DSPGCSProductionCommandProviderSubsystem.generated.h"

class UDSPGCSProductionTelemetryProviderSubsystem;

/** Unified UE command API for the production DSP-GCS channel. */
UCLASS()
class DRONESHOWPLATFORM1_API UDSPGCSProductionCommandProviderSubsystem final
	: public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|DSP-GCS Command")
	FString SubmitCommand(
		EGCSProductionCommandType CommandType,
		const TArray<FString>& TargetDroneIDs,
		const FGCSProductionCommandParameters& Parameters);

	UFUNCTION(BlueprintCallable, Category = "Drone GCS|DSP-GCS Command")
	void SetControlMode(EGCSGroundStationControlMode NewMode);

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Command")
	EGCSGroundStationControlMode GetControlMode() const { return ControlMode; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Command")
	bool IsEnabled() const { return bEnabled && bSafeTestMode; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Command")
	FGCSProductionCommandResult GetLastResult() const { return LastResult; }

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|DSP-GCS Command")
	FGCSProductionCommandResultEvent OnCommandResult;

private:
	struct FPendingCommand
	{
		EGCSProductionCommandType Type = EGCSProductionCommandType::Arm;
		FString Source;
		TSet<FString> PendingDroneIDs;
	};

	void HandleResultPayload(
		const dspgcs::MessageEnvelope& Envelope,
		const std::vector<std::uint8_t>& Payload);
	void ApplyResultBatch(
		dspgcs::MessageEnvelope Envelope,
		dspgcs::CommandResultBatchDTO Batch);
	FString NextCommandID();

	TWeakObjectPtr<UDSPGCSProductionTelemetryProviderSubsystem> Channel;
	TMap<FString, FPendingCommand> PendingCommands;
	FGCSProductionCommandResult LastResult;
	bool bEnabled = false;
	bool bSafeTestMode = true;
	int64 Sequence = 0;
	int32 CommandCounter = 0;
	EGCSGroundStationControlMode ControlMode = EGCSGroundStationControlMode::Manual;
};
