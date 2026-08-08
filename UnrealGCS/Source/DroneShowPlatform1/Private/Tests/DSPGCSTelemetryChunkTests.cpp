#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSServerAdapterSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSTelemetryChunkAssemblyTest,
	"DroneGCS.Data.DSPGCSTelemetryChunkAssembly.AtomicThousandDroneSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSTelemetryChunkAssemblyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}
	GameInstance->InitializeStandalone(TEXT("DSPGCSTelemetryChunkTestWorld"));
	UFleetManagerSubsystem* Fleet =
		GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	UGCSServerAdapterSubsystem* Adapter =
		GameInstance->GetSubsystem<UGCSServerAdapterSubsystem>();
	UDSPGCSProductionTelemetryProviderSubsystem* Provider =
		GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>();
	if (!TestNotNull(TEXT("FleetManager subsystem initialized"), Fleet)
		|| !TestNotNull(TEXT("ServerAdapter subsystem initialized"), Adapter)
		|| !TestNotNull(TEXT("Telemetry provider subsystem initialized"), Provider))
	{
		GameInstance->Shutdown();
		return false;
	}

	dspgcs::MessageEnvelope Envelope;
	Envelope.messageType = dspgcs::MessageType::TelemetryBatch;
	Envelope.sourceId = "CONSOLE-TEST";
	Envelope.sessionId = "THOUSAND-DRONE-TEST";
	Envelope.sequence = 42;
	Envelope.fleetRevision = 42;
	Envelope.timestampUtcMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;

	const auto DispatchFromWorker = [Provider](dspgcs::TransportFrame Frame)
	{
		TFuture<void> Worker = Async(
			EAsyncExecution::ThreadPool,
			[Provider, Frame = MoveTemp(Frame)]() mutable
			{
				Provider->HandleTransportFrame(MoveTemp(Frame));
			});
		Worker.Wait();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(
			ENamedThreads::GameThread);
	};

	for (std::uint32_t ChunkIndex = 0; ChunkIndex < 3; ++ChunkIndex)
	{
		dspgcs::TelemetryBatchDTO Chunk;
		Chunk.batchId = "SNAPSHOT-42";
		Chunk.batchMode = dspgcs::BatchMode::Snapshot;
		Chunk.fleetRevision = 42;
		Chunk.snapshotId = "SNAPSHOT-42";
		Chunk.chunkIndex = ChunkIndex;
		Chunk.chunkCount = 3;

		const int32 FirstDroneIndex = static_cast<int32>(ChunkIndex) * 400;
		const int32 ChunkDroneCount = ChunkIndex < 2 ? 400 : 200;
		Chunk.items.reserve(ChunkDroneCount);
		for (int32 ItemIndex = 0; ItemIndex < ChunkDroneCount; ++ItemIndex)
		{
			dspgcs::TelemetryDTO Item;
			Item.droneId = "Drone" +
				std::to_string(FirstDroneIndex + ItemIndex + 1);
			Chunk.items.push_back(MoveTemp(Item));
		}

		dspgcs::TransportFrame Frame;
		Frame.bytes = dspgcs::EncodeTelemetry(Envelope, Chunk);
		DispatchFromWorker(MoveTemp(Frame));

		if (ChunkIndex < 2)
		{
			TestEqual(
				TEXT("Partial snapshot never reaches FleetManager"),
				Fleet->GetDroneCount(),
				0);
			TestEqual(
				TEXT("Partial snapshot emits no batch event"),
				Fleet->DataBatchAppliedBroadcastCountForTests,
				0);
		}
	}

	TestEqual(
		TEXT("All drones reach FleetManager atomically"),
		Fleet->GetDroneCount(),
		1000);
	TestEqual(
		TEXT("Complete snapshot applies exactly once"),
		Fleet->DataBatchAppliedBroadcastCountForTests,
		1);
	TestEqual(
		TEXT("Provider records the complete snapshot count"),
		Provider->GetReceivedDroneCount(),
		1000);
	TestTrue(
		TEXT("Complete valid snapshot keeps telemetry link healthy"),
		Provider->IsTelemetryLinkConnected());

	const TArray<FDroneData> OrderedDrones = Fleet->GetDronePage(0, 1000);
	if (OrderedDrones.Num() == 1000)
	{
		TestEqual(
			TEXT("First stable-order drone"),
			OrderedDrones[0].DroneID,
			FString(TEXT("Drone1")));
		TestEqual(
			TEXT("Last stable-order drone"),
			OrderedDrones.Last().DroneID,
			FString(TEXT("Drone1000")));
	}

	GameInstance->Shutdown();
	return true;
}

#endif
