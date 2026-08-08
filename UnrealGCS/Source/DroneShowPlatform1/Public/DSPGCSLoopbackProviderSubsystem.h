/**
 * @brief Stage 4.2 DSP-GCS 本地验收 Provider。
 *
 * 分类：Acceptance / Loopback Test。
 * 当前 [DSPGCS.Transport] Enabled=False，不参与 Production 数据链。
 * 注意：不要与当前 Production 正在使用的 DSPGCSLoopbackTransport 混淆。
 */
#pragma once

#include "CoreMinimal.h"
#include "GCSDataProvider.h"
#include "IDSPGCSTransport.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include <memory>

#include "DSPGCSLoopbackProviderSubsystem.generated.h"

class UGCSServerAdapterSubsystem;

/**
 * Stage 4.2 localhost-only DSP-GCS acceptance provider.
 *
 * This provider translates independent protocol DTOs into the UE fleet data
 * port. It never references MAVLink, DroneSession, or ConsoleGCS internal
 * types, and it is disabled unless explicitly enabled in DefaultGame.ini.
 */
UCLASS()
class DRONESHOWPLATFORM1_API UDSPGCSLoopbackProviderSubsystem final
	: public UGameInstanceSubsystem
	, public IGCSDataProvider
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual FName GetProviderID() const override;
	virtual bool Start(UGCSServerAdapterSubsystem& Adapter) override;
	virtual void Stop() override;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Test")
	int32 GetReceivedTelemetryCount() const { return ReceivedTelemetryCount; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Test")
	int32 GetReceivedResultCount() const { return ReceivedResultCount; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Test")
	bool HasPassedLoopbackAcceptance() const { return bAcceptancePassed; }

private:
	bool CreateTransport();
	void HandleTransportFrame(dspgcs::TransportFrame Frame);
	void SendAcceptanceCommands();
	bool SendCommandFrame(
		const FString& CommandID,
		uint16 CommandType,
		const TArray<FString>& TargetDroneIDs,
		bool bStart);
	void EvaluateAcceptance();

	TWeakObjectPtr<UGCSServerAdapterSubsystem> AdapterRef;
	std::unique_ptr<dspgcs::IDSPGCSTransport> Transport;

	bool bEnabled = false;
	bool bSafeTestMode = true;
	bool bCommandsSent = false;
	bool bAcceptancePassed = false;
	int32 ReceivedTelemetryCount = 0;
	int32 ReceivedResultCount = 0;
	int64 OutgoingSequence = 0;
	FString ArmCommandID;
	FString HoverCommandID;
	TSet<FString> ArmResultDroneIDs;
	TSet<FString> HoverResultDroneIDs;
};
