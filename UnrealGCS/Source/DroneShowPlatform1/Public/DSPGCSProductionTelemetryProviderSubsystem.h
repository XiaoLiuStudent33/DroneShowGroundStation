#pragma once

#include "CoreMinimal.h"
#include "DSPGCSProtocol.h"
#include "GCSDataProvider.h"
#include "IDSPGCSTransport.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include <memory>
#include <functional>
#include <vector>

#include "DSPGCSProductionTelemetryProviderSubsystem.generated.h"

class UGCSServerAdapterSubsystem;
namespace dspgcs { struct MessageEnvelope; }

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

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual FName GetProviderID() const override;
	virtual bool Start(UGCSServerAdapterSubsystem& Adapter) override;
	virtual void Stop() override;

	UFUNCTION(BlueprintPure, Category = "Drone GCS|DSP-GCS Telemetry")
	int32 GetReceivedDroneCount() const { return ReceivedDroneCount; }

	/** Shared production Transport output used by the command provider. */
	bool SendFrame(const std::vector<std::uint8_t>& Frame);
	void SetCommandResultHandler(FCommandResultHandler Handler);

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FDSPGCSTelemetryChunkAssemblyTest;
#endif

	bool CreateTransport();
	void HandleTransportFrame(dspgcs::TransportFrame Frame);
	bool TryAssembleTelemetrySnapshot(
		const dspgcs::MessageEnvelope& Envelope,
		dspgcs::TelemetryBatchDTO&& Chunk,
		dspgcs::TelemetryBatchDTO& CompleteBatch);
	void ResetPendingTelemetrySnapshot();

	TWeakObjectPtr<UGCSServerAdapterSubsystem> AdapterRef;
	std::unique_ptr<dspgcs::IDSPGCSTransport> Transport;
	FCriticalSection CommandResultHandlerMutex;
	FCommandResultHandler CommandResultHandler;
	bool bEnabled = false;
	int32 ReceivedDroneCount = 0;
	std::string PendingSnapshotSessionID;
	std::string PendingSnapshotID;
	std::uint64_t PendingSnapshotFleetRevision = 0;
	std::vector<std::vector<dspgcs::TelemetryDTO>> PendingSnapshotChunks;
	std::vector<bool> PendingSnapshotChunksReceived;
	std::size_t PendingSnapshotReceivedCount = 0;
};
