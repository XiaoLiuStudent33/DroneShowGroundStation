#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"

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
	UDSPGCSProductionTelemetryProviderSubsystem* Provider =
		NewObject<UDSPGCSProductionTelemetryProviderSubsystem>(GameInstance);
	if (!TestNotNull(TEXT("Telemetry provider created"), Provider))
	{
		return false;
	}

	dspgcs::MessageEnvelope Envelope;
	Envelope.sessionId = "THOUSAND-DRONE-TEST";
	Envelope.fleetRevision = 42;

	dspgcs::TelemetryBatchDTO CompleteBatch;
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

		const bool bComplete = Provider->TryAssembleTelemetrySnapshot(
			Envelope,
			MoveTemp(Chunk),
			CompleteBatch);
		TestEqual(
			TEXT("Snapshot is exposed only after its final chunk"),
			bComplete,
			ChunkIndex == 2);
	}

	TestEqual(
		TEXT("All drones survive snapshot assembly"),
		static_cast<int32>(CompleteBatch.items.size()),
		1000);
	if (CompleteBatch.items.size() == 1000)
	{
		TestEqual(
			TEXT("First stable-order drone"),
			FString(UTF8_TO_TCHAR(CompleteBatch.items.front().droneId.c_str())),
			FString(TEXT("Drone1")));
		TestEqual(
			TEXT("Last stable-order drone"),
			FString(UTF8_TO_TCHAR(CompleteBatch.items.back().droneId.c_str())),
			FString(TEXT("Drone1000")));
	}

	return true;
}

#endif
