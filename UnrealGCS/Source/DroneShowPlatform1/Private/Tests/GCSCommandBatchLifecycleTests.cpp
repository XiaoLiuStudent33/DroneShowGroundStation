#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "DSPGCSProductionCommandProviderSubsystem.h"
#include "DSPGCSCommandBatching.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSCommandBatchLifecycleTest,
    "DroneGCS.Command.BatchLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSCommandBatchLifecycleTest::RunTest(const FString&)
{
    auto* Instance = NewObject<UGameInstance>();
    auto* Provider = NewObject<UDSPGCSProductionCommandProviderSubsystem>(Instance);
    UDSPGCSProductionCommandProviderSubsystem::FPendingCommand Pending;
    Pending.OperationID = TEXT("parent");
    Pending.PendingDroneIDs = {TEXT("Drone000"), TEXT("Drone001")};
    Provider->PendingCommands.Add(TEXT("child"), Pending);
    FGCSProductionCommandResult Received;
    Received.DroneID = TEXT("Drone000");
    Received.bFinal = false;
    Provider->ApplyResultBatch(TEXT("child"), {Received}, false);
    TestEqual(TEXT("Progress does not consume final result ownership"),
        Provider->PendingCommands[TEXT("child")].PendingDroneIDs.Num(), 2);
    Received.bFinal = true;
    Provider->ApplyResultBatch(TEXT("child"), {Received}, false);
    TestEqual(TEXT("Child results use operation ID"), Provider->GetLastResult().CommandID, FString(TEXT("parent")));
    TestEqual(TEXT("Only completed vehicle removed"), Provider->PendingCommands[TEXT("child")].PendingDroneIDs.Num(), 1);

    // No channel: exercise local batch failure without network or aircraft.
    UDSPGCSProductionCommandProviderSubsystem::FQueuedBatch Batch;
    Batch.CommandID = TEXT("child");
    Batch.Bytes.Add(1);
    Provider->QueuedBatches.push_back(MoveTemp(Batch));
    Provider->PumpCommandBatches(0);
    TestEqual(TEXT("Unsent remainder reports explicit failure"), Provider->GetLastResult().ErrorCode, FString(TEXT("LOCAL_SEND_FAILED")));
    TestTrue(TEXT("Failed child is not retried"), Provider->QueuedBatches.empty());
    TestTrue(TEXT("Child completed once"), Provider->PendingCommands.IsEmpty());
    Provider->PumpCommandBatches(0);

    dspgcs::CommandRequestDTO Request;
    Request.commandId = "parent";
    for (int i = 0; i < 20000; ++i) Request.targetDroneIds.push_back("Drone" + std::to_string(i));
    dspgcs::MessageEnvelope Envelope;
    Envelope.correlationId = Request.commandId;
    auto Parts = dspgcs::SplitCommandTargets(Envelope, Request);
    TestEqual(TEXT("20K uses bounded dispatch batches"), static_cast<int32>(Parts.size()), 79);
    int32 Count = 0;
    for (const auto& Part : Parts)
    {
        Envelope.correlationId = Part.commandId;
        TestTrue(TEXT("Encoded child fits datagram"), dspgcs::EncodeCommand(Envelope, Part).size() <= 60000);
        Count += static_cast<int32>(Part.targetDroneIds.size());
    }
    TestEqual(TEXT("Frozen targets all retained"), Count, 20000);
    return true;
}
#endif
