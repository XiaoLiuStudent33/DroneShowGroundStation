#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "../GCSCommandResultConversion.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSCommandResultContractTest,
	"DroneGCS.Command.StableResultContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSCommandResultContractTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		dspgcs::AckState Wire;
		EGCSCommandResultState State;
		const TCHAR* Text;
		bool bSuccess;
	};
	const FCase Cases[] =
	{
		{ dspgcs::AckState::Received, EGCSCommandResultState::Received, TEXT("RECEIVED"), false },
		{ dspgcs::AckState::Validated, EGCSCommandResultState::Validated, TEXT("VALIDATED"), false },
		{ dspgcs::AckState::Queued, EGCSCommandResultState::Queued, TEXT("QUEUED"), false },
		{ dspgcs::AckState::Sent, EGCSCommandResultState::Sent, TEXT("SENT"), true },
		{ dspgcs::AckState::InProgress, EGCSCommandResultState::InProgress, TEXT("IN_PROGRESS"), false },
		{ dspgcs::AckState::Accepted, EGCSCommandResultState::Accepted, TEXT("ACCEPTED"), true },
		{ dspgcs::AckState::Rejected, EGCSCommandResultState::Rejected, TEXT("REJECTED"), false },
		{ dspgcs::AckState::Failed, EGCSCommandResultState::Failed, TEXT("FAILED"), false },
		{ dspgcs::AckState::TimedOut, EGCSCommandResultState::TimedOut, TEXT("TIMEOUT"), false },
		{ dspgcs::AckState::Cancelled, EGCSCommandResultState::Cancelled, TEXT("CANCELLED"), false },
		{ dspgcs::AckState::Simulated, EGCSCommandResultState::Simulated, TEXT("SIMULATED"), true },
		{ static_cast<dspgcs::AckState>(255), EGCSCommandResultState::Unknown, TEXT("UNKNOWN"), false }
	};
	for (const FCase& Case : Cases)
	{
		for (const bool bFinal : { false, true })
		{
			dspgcs::DroneCommandResultDTO Wire;
			Wire.commandId = "item-id-is-not-batch-id";
			Wire.droneId = "Drone007";
			Wire.ackState = Case.Wire;
			Wire.error.code = "EXISTING_CODE";
			Wire.error.message = "existing diagnostic";
			Wire.final = bFinal;
			FGCSProductionCommandResult Result =
				GCSCommandResultConversion::ToResult(TEXT("batch-id"), Wire);
			TestEqual(TEXT("Batch identity remains authoritative"), Result.CommandID, FString(TEXT("batch-id")));
			TestEqual(TEXT("Target ID preserved"), Result.DroneID, FString(TEXT("Drone007")));
			TestEqual(TEXT("Explicit stable state mapping"), Result.State, Case.State);
			TestEqual(TEXT("Existing display text unchanged"), Result.AckState, FString(Case.Text));
			TestEqual(TEXT("Error code preserved"), Result.ErrorCode, FString(TEXT("EXISTING_CODE")));
			TestEqual(TEXT("Diagnostic preserved"), Result.Detail, FString(TEXT("existing diagnostic")));
			TestEqual(TEXT("Final flag preserved, not inferred"), Result.bFinal, bFinal);
			TestEqual(TEXT("Simulation flag preserved"), Result.bSimulated, Case.State == EGCSCommandResultState::Simulated);
			TestEqual(TEXT("Same dispatch policy for every state"), Result.IsSuccessfulDispatch(), Case.bSuccess);
			Result.AckState = Case.bSuccess ? TEXT("REJECTED") : TEXT("ACCEPTED");
			TestEqual(TEXT("Display cannot control business decisions"), Result.IsSuccessfulDispatch(), Case.bSuccess);
		}
	}
	TestFalse(TEXT("Unspecified state cannot grant success"), FGCSProductionCommandResult{}.IsSuccessfulDispatch());
	return true;
}
#endif
