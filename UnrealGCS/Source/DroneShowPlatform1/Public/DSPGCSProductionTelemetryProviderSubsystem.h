/**
 * @brief UE 侧 DSP-GCS Production 遥测 Provider 与 Snapshot 分片重组状态。
 *
 * 分类：Production。
 * 输入：Console DSP-GCS Telemetry Frame；输出：完整 FGCSFleetSnapshot 到 GCSServerAdapter。
 * 关键约束：所有分片完整前不得向 FleetManager 提交部分 Snapshot。
 */
#pragma once

#include "CoreMinimal.h"
#include "GCSTelemetryTypes.h"
#include "Containers/Ticker.h"
#include "DSPGCSProtocol.h"
#include "GCSDataProvider.h"
#include "IDSPGCSTransport.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include <memory>
#include <atomic>
#include <functional>
#include <vector>

#include "DSPGCSProductionTelemetryProviderSubsystem.generated.h"

class UGCSServerAdapterSubsystem;
namespace dspgcs { struct MessageEnvelope; struct FrozenAssignmentAckDTO; }

/**
 * Production DSP-GCS channel owner. Telemetry remains an IGCSDataProvider;
 * the command provider shares this Factory-created Transport for requests and
 * result frames so UE never binds a duplicate socket.
 */
UCLASS()
class DRONESHOWPLATFORM1_API UDSPGCSProductionTelemetryProviderSubsystem final
	: public UGameInstanceSubsystem
	, public IGCSDataProvider
{
	GENERATED_BODY()

public:
	using FCommandResultHandler = std::function<void(
		const dspgcs::MessageEnvelope&,
		const std::vector<std::uint8_t>&)>;
	using FFrozenAssignmentAckHandler = std::function<void(
		const dspgcs::MessageEnvelope&,
		const dspgcs::FrozenAssignmentAckDTO&)>;
	using FUploadJobFrameHandler = std::function<void(
		const dspgcs::MessageEnvelope&,
		const std::vector<std::uint8_t>&)>;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual FName GetProviderID() const override;
	virtual bool Start(UGCSServerAdapterSubsystem& Adapter) override;
	virtual void Stop() override;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Telemetry")
	int32 GetReceivedDroneCount() const { return ReceivedDroneCount.load(); }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Telemetry")
	EDSPGCSTelemetryLinkState GetTelemetryLinkState() const;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Telemetry")
	bool IsTelemetryLinkConnected() const;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Telemetry")
	bool IsEnabled() const { return bEnabled; }

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Telemetry")
	int32 GetTelemetryTimeoutMs() const { return TelemetryTimeoutMs; }

	/** Latest complete #187 table snapshot. Standard fleet telemetry is separate. */
	TArray<FMDHKTelemetryTableRow> GetMDHKTelemetryRows() const;

	UPROPERTY(BlueprintAssignable, Category = "Drone GCS|DSP-GCS Telemetry")
	FDSPGCSTelemetryLinkStateChangedEvent OnTelemetryLinkStateChanged;

	/** Shared production Transport output used by the command provider. */
	bool SendFrame(const std::vector<std::uint8_t>& Frame);
	void SetCommandResultHandler(FCommandResultHandler Handler);
	void SetFrozenAssignmentAckHandler(FFrozenAssignmentAckHandler Handler);
	void SetUploadJobFrameHandler(FUploadJobFrameHandler Handler);

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FDSPGCSTelemetryChunkAssemblyTest;
	friend class FDSPGCSTelemetryLinkHealthTest;
	friend class FGCSSelectionStateSeparationTest;
	friend class FRouteGpsCaptureProductionTelemetryPreservationTest;
#endif

	bool CreateTransport();
	void HandleTransportFrame(dspgcs::TransportFrame Frame);
	bool TryAssembleTelemetrySnapshot(
		const dspgcs::MessageEnvelope& Envelope,
		dspgcs::TelemetryBatchDTO&& Chunk,
		dspgcs::TelemetryBatchDTO& CompleteBatch);
	void ResetPendingTelemetrySnapshot();
	void StartTelemetryHealthMonitor();
	void StopTelemetryHealthMonitor();
	bool TickTelemetryHealth(float DeltaSeconds);
	void MarkValidTelemetryReceivedAt(double ReceiveTimeSeconds);
	void EvaluateTelemetryLinkHealthAt(double CurrentTimeSeconds);
	EDSPGCSTelemetryLinkState GetTelemetryLinkStateAt(
		double CurrentTimeSeconds) const;
	void SetTelemetryLinkState(EDSPGCSTelemetryLinkState NewState);

	TWeakObjectPtr<UGCSServerAdapterSubsystem> AdapterRef;
	std::unique_ptr<dspgcs::IDSPGCSTransport> Transport;
	FCriticalSection CommandResultHandlerMutex;
	FCommandResultHandler CommandResultHandler;
	FCriticalSection FrozenAssignmentAckHandlerMutex;
	FFrozenAssignmentAckHandler FrozenAssignmentAckHandler;
	FCriticalSection UploadJobFrameHandlerMutex;
	FUploadJobFrameHandler UploadJobFrameHandler;
	mutable FCriticalSection MDHKTelemetryRowsMutex;
	TArray<FMDHKTelemetryTableRow> MDHKTelemetryRows;
	bool bEnabled = false;
	std::atomic<int32> ReceivedDroneCount{0};
	int32 TelemetryTimeoutMs = 2000;
	std::atomic<double> LastValidTelemetryReceiveTimeSeconds{0.0};
	EDSPGCSTelemetryLinkState TelemetryLinkState =
		EDSPGCSTelemetryLinkState::Disconnected;
	FTSTicker::FDelegateHandle TelemetryHealthTickerHandle;
	std::string PendingSnapshotSessionID;
	std::string PendingSnapshotID;
	std::uint64_t PendingSnapshotFleetRevision = 0;
	std::vector<std::vector<dspgcs::TelemetryDTO>> PendingSnapshotChunks;
	std::vector<bool> PendingSnapshotChunksReceived;
	std::size_t PendingSnapshotReceivedCount = 0;

	static constexpr int32 MinimumTelemetryTimeoutMs = 250;
	static constexpr int32 MaximumTelemetryTimeoutMs = 60000;
	static constexpr float TelemetryHealthCheckIntervalSeconds = 0.25f;
};
