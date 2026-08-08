#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSUIBridgeSubsystem.h"
#include "HAL/PlatformTime.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSTelemetryLinkHealthTest,
	"DroneGCS.Data.DSPGCSTelemetryLink.HealthTimeoutFleetRetentionAndRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSTelemetryLinkHealthTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}
	UFleetManagerSubsystem* Fleet =
		NewObject<UFleetManagerSubsystem>(GameInstance);
	UDSPGCSProductionTelemetryProviderSubsystem* Provider =
		NewObject<UDSPGCSProductionTelemetryProviderSubsystem>(GameInstance);
	if (!TestNotNull(TEXT("FleetManager created"), Fleet) ||
		!TestNotNull(TEXT("Telemetry provider created"), Provider))
	{
		return false;
	}

	for (int32 Index = 1; Index <= 3; ++Index)
	{
		FDroneData Drone;
		Drone.DroneID = FString::Printf(TEXT("Drone%03d"), Index);
		Drone.bOnline = Index != 2;
		TestTrue(TEXT("Drone added to retained fleet"), Fleet->AddDrone(Drone));
	}
	const TArray<FDroneData> InitialOrder = Fleet->GetDronePage(0, 10);

	TestEqual(
		TEXT("UE telemetry timeout fallback is 2000ms"),
		Provider->GetTelemetryTimeoutMs(),
		2000);
	TestEqual(
		TEXT("Telemetry link starts disconnected"),
		Provider->GetTelemetryLinkState(),
		EDSPGCSTelemetryLinkState::Disconnected);

	dspgcs::TransportFrame InvalidFrame;
	Provider->HandleTransportFrame(MoveTemp(InvalidFrame));
	TestEqual(
		TEXT("Invalid frame does not refresh telemetry link"),
		Provider->GetTelemetryLinkState(),
		EDSPGCSTelemetryLinkState::Disconnected);

	const double ReceiveTimeSeconds = FPlatformTime::Seconds();
	Provider->MarkValidTelemetryReceivedAt(ReceiveTimeSeconds);
	TestEqual(
		TEXT("Valid complete snapshot marks server connected"),
		Provider->GetTelemetryLinkStateAt(ReceiveTimeSeconds),
		EDSPGCSTelemetryLinkState::Connected);
	TestEqual(
		TEXT("Connected online drone displays Online"),
		UGCSUIBridgeSubsystem::ResolveDroneDisplayStatus(
			EDSPGCSTelemetryLinkState::Connected,
			true),
		EGCSDroneDisplayStatus::Online);
	TestEqual(
		TEXT("Connected offline drone displays Offline"),
		UGCSUIBridgeSubsystem::ResolveDroneDisplayStatus(
			EDSPGCSTelemetryLinkState::Connected,
			false),
		EGCSDroneDisplayStatus::Offline);

	Provider->EvaluateTelemetryLinkHealthAt(
		ReceiveTimeSeconds + 2.001);
	TestEqual(
		TEXT("Telemetry timeout marks server disconnected"),
		Provider->TelemetryLinkState,
		EDSPGCSTelemetryLinkState::Disconnected);
	TestEqual(
		TEXT("Last online drone displays Link Lost while server is disconnected"),
		UGCSUIBridgeSubsystem::ResolveDroneDisplayStatus(
			EDSPGCSTelemetryLinkState::Disconnected,
			true),
		EGCSDroneDisplayStatus::LinkLost);
	TestEqual(TEXT("Fleet count survives link loss"), Fleet->GetDroneCount(), 3);

	FDroneData RetainedDrone;
	TestTrue(
		TEXT("Raw Drone001 remains in fleet"),
		Fleet->GetDroneByID(TEXT("Drone001"), RetainedDrone));
	TestTrue(
		TEXT("Raw Drone001 bOnline remains true"),
		RetainedDrone.bOnline);
	const TArray<FDroneData> DisconnectedOrder = Fleet->GetDronePage(0, 10);
	TestEqual(
		TEXT("DroneOrder count survives link loss"),
		DisconnectedOrder.Num(),
		InitialOrder.Num());
	for (int32 Index = 0; Index < InitialOrder.Num(); ++Index)
	{
		TestEqual(
			*FString::Printf(TEXT("DroneOrder index %d remains stable"), Index),
			DisconnectedOrder[Index].DroneID,
			InitialOrder[Index].DroneID);
	}

	const double RecoveryTimeSeconds = ReceiveTimeSeconds + 3.0;
	Provider->MarkValidTelemetryReceivedAt(RecoveryTimeSeconds);
	TestEqual(
		TEXT("Next valid snapshot restores server connection"),
		Provider->GetTelemetryLinkStateAt(RecoveryTimeSeconds),
		EDSPGCSTelemetryLinkState::Connected);
	TestEqual(
		TEXT("Recovered online drone displays Online"),
		UGCSUIBridgeSubsystem::ResolveDroneDisplayStatus(
			EDSPGCSTelemetryLinkState::Connected,
			true),
		EGCSDroneDisplayStatus::Online);
	TestEqual(
		TEXT("Recovered offline drone displays Offline"),
		UGCSUIBridgeSubsystem::ResolveDroneDisplayStatus(
			EDSPGCSTelemetryLinkState::Connected,
			false),
		EGCSDroneDisplayStatus::Offline);
	TestEqual(TEXT("Fleet count survives recovery"), Fleet->GetDroneCount(), 3);

	return true;
}

#endif
