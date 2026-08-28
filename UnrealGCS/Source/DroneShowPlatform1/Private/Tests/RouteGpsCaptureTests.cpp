#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DroneData.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "DSPGCSProtocol.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSDataTypes.h"
#include "GCSUIBridgeSubsystem.h"
#include "HAL/PlatformTime.h"
#include "Async/TaskGraphInterfaces.h"
#include "RouteGpsCaptureSubsystem.h"

namespace RouteGpsCaptureTests
{
	FRouteGpsCapturePolicy MakePolicy()
	{
		FRouteGpsCapturePolicy Policy;
		Policy.ProfileName = TEXT("AutomationTest");
		Policy.bHardwareValidated = false;
		Policy.RequiredFixType = 6;
		Policy.MaxSampleAgeMilliseconds = 500;
		Policy.MinWindowDurationMilliseconds = 400;
		Policy.MinUniqueSamples = 3;
		Policy.MaxMedianResidualMeters = 0.25;
		Policy.MaxRobustSpreadMeters = 0.25;
		Policy.MaxSampleResidualMeters = 0.5;
		Policy.MaxJumpDistanceMeters = 2.0;
		Policy.CaptureTimeoutMilliseconds = 2000;
		return Policy;
	}

	FGCSFleetSnapshot MakeFleet(
		const int32 Count,
		const FString& SessionID = TEXT("F1-FLEET"),
		const int32 OfflineIndex = INDEX_NONE,
		const int32 EndpointChangedIndex = INDEX_NONE)
	{
		FGCSFleetSnapshot Snapshot;
		Snapshot.Header.SchemaVersion = GCSDataSchema::CurrentVersion;
		Snapshot.Header.SourceID = TEXT("F1-TEST");
		Snapshot.Header.SessionID = SessionID;
		Snapshot.FleetRevision = 1;
		for (int32 Index = 1; Index <= Count; ++Index)
		{
			FDroneData Drone;
			Drone.DroneID = FString::Printf(TEXT("Drone%03d"), Index);
			Drone.EndpointIdentity = FString::Printf(
				Index == EndpointChangedIndex
					? TEXT("127.0.1.%d:14550") : TEXT("127.0.0.%d:14550"),
				Index);
			Drone.bOnline = Index != OfflineIndex;
			Snapshot.Drones.Add(MoveTemp(Drone));
		}
		return Snapshot;
	}

	FRouteGpsTelemetryObservation MakeObservation(
		URouteGpsCaptureSubsystem& Capture,
		const FString& DroneID,
		const int32 DroneIndex,
		const int64 TimestampUtcMs,
		const double MonotonicSeconds,
		const double LatitudeOffsetDegrees = 0.0,
		const int32 FixType = 6,
		const FString& RequestID = FString(),
		const FString& EndpointOverride = FString())
	{
		FRouteGpsTelemetryObservation Observation;
		Observation.CaptureRequestID = RequestID.IsEmpty()
			? Capture.GetCurrentCaptureRequestID() : RequestID;
		Observation.DroneID = DroneID;
		Observation.EndpointIdentity = EndpointOverride.IsEmpty()
			? FString::Printf(TEXT("127.0.0.%d:14550"), DroneIndex)
			: EndpointOverride;
		Observation.bVehicleOnline = true;
		Observation.bGpsValid = FixType >= 3;
		Observation.FixType = FixType;
		Observation.Latitude = 31.932755 + DroneIndex * 0.0001
			+ LatitudeOffsetDegrees;
		Observation.Longitude = 118.638247 + DroneIndex * 0.0001;
		Observation.AltitudeAMSL = 100.0f + DroneIndex;
		Observation.bAltitudeValid = true;
		Observation.GpsSampleTimestampUtcMs = TimestampUtcMs;
		Observation.TelemetryReceivedUtcMs = TimestampUtcMs;
		Observation.TelemetryReceivedMonotonicSeconds = MonotonicSeconds;
		return Observation;
	}

	void Shutdown(UGameInstance* GameInstance)
	{
		if (GameInstance)
		{
			GameInstance->Shutdown();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteGpsCaptureConfiguredPolicyTest,
	"DroneGCS.Data.RouteGpsCapture.F1.ConfiguredPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteGpsCaptureConfiguredPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteGpsCapturePolicy Policy;
	FString Error;
	TestTrue(TEXT("Configured F.1 policy loads and validates"),
		URouteGpsCaptureSubsystem::LoadConfiguredPolicy(Policy, Error));
	TestEqual(TEXT("Commissioning profile identity is explicit"),
		Policy.ProfileName, FString(TEXT("SimulationCommissioningV1")));
	TestFalse(TEXT("Commissioning profile is not represented as hardware validated"),
		Policy.bHardwareValidated);
	TestTrue(TEXT("Stable window requires multiple unique samples"),
		Policy.MinUniqueSamples >= 2);
	TestTrue(TEXT("Capture timeout contains the complete stability window"),
		Policy.CaptureTimeoutMilliseconds >= Policy.MinWindowDurationMilliseconds);
	if (!Error.IsEmpty()) AddError(Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteGpsCaptureProductionTelemetryPreservationTest,
	"DroneGCS.Data.RouteGpsCapture.F1.ProductionTelemetryPreservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteGpsCaptureProductionTelemetryPreservationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace RouteGpsCaptureTests;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteGpsCaptureProductionTelemetry"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	UGCSUIBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
	URouteGpsCaptureSubsystem* Capture =
		GameInstance->GetSubsystem<URouteGpsCaptureSubsystem>();
	UDSPGCSProductionTelemetryProviderSubsystem* Provider =
		GameInstance->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>();
	if (!Fleet || !Bridge || !Capture || !Provider)
	{
		Shutdown(GameInstance);
		return false;
	}
	TestTrue(TEXT("Initial identity Fleet applied"),
		Fleet->ApplySnapshot(MakeFleet(1, TEXT("F1-PRODUCTION-TELEMETRY"))));
	Bridge->ReplaceCommandDroneSelection({ TEXT("Drone001") });
	FString Error;
	TestTrue(TEXT("Production telemetry capture begins"),
		Capture->BeginCapture(MakePolicy(), Error));

	const FDateTime Now = FDateTime::UtcNow();
	const int64 GpsReceiveUtcMs = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	const auto Dispatch = [Provider, GpsReceiveUtcMs](
		const int64 TimestampUtcMs,
		const uint64 Sequence,
		const double LatitudeOffset)
	{
		dspgcs::MessageEnvelope Envelope;
		Envelope.messageType = dspgcs::MessageType::TelemetryBatch;
		Envelope.sourceId = "F1-CONSOLE";
		Envelope.sessionId = "F1-PRODUCTION-TELEMETRY";
		Envelope.sequence = Sequence;
		Envelope.fleetRevision = Sequence;
		Envelope.timestampUtcMs = GpsReceiveUtcMs;
		dspgcs::TelemetryBatchDTO Batch;
		Batch.batchId = "F1-BATCH";
		Batch.batchMode = dspgcs::BatchMode::Snapshot;
		Batch.fleetRevision = Sequence;
		Batch.snapshotId = "F1-SNAPSHOT-" + std::to_string(Sequence);
		Batch.chunkIndex = 0;
		Batch.chunkCount = 1;
		dspgcs::TelemetryDTO Item;
		Item.droneId = "Drone001";
		Item.sysId = 1;
		Item.sampleTimestampUtcMs = TimestampUtcMs;
		Item.gps.valid = true;
		Item.gps.fixType = 6;
		Item.gps.latitudeDeg = 31.932855 + LatitudeOffset;
		Item.gps.longitudeDeg = 118.638347;
		Item.altitude.valid = true;
		Item.altitude.amslMeters = 101.0f;
		Item.connection.online = true;
		Item.connection.linkId = "127.0.0.1:14550";
		Batch.items.push_back(MoveTemp(Item));
		dspgcs::TransportFrame Frame;
		Frame.bytes = dspgcs::EncodeTelemetry(Envelope, Batch);
		Provider->HandleTransportFrame(MoveTemp(Frame));
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	};

	Dispatch(GpsReceiveUtcMs, 1, -0.0000001);
	FRouteGpsCaptureSnapshot Snapshot;
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("UE preserves Console GPS receive timestamp"),
		Snapshot.Participants[0].LastGpsSampleUtcMs, GpsReceiveUtcMs);
	TestEqual(TEXT("First production DTO creates one unique GPS sample"),
		Snapshot.Participants[0].UniqueSampleCount, 1);
	Dispatch(GpsReceiveUtcMs, 2, -0.0000001);
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Repeated production DTO keeps one unique GPS sample"),
		Snapshot.Participants[0].UniqueSampleCount, 1);
	Dispatch(GpsReceiveUtcMs + 200, 3, 0.0);
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("New production GPS timestamp creates second sample"),
		Snapshot.Participants[0].UniqueSampleCount, 2);
	Shutdown(GameInstance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteGpsCaptureSelectionStableWindowTest,
	"DroneGCS.Data.RouteGpsCapture.F1.SelectionStableWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteGpsCaptureSelectionStableWindowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace RouteGpsCaptureTests;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteGpsCaptureSelectionStableWindow"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	UGCSUIBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
	URouteGpsCaptureSubsystem* Capture =
		GameInstance->GetSubsystem<URouteGpsCaptureSubsystem>();
	if (!TestNotNull(TEXT("Fleet"), Fleet) || !TestNotNull(TEXT("Bridge"), Bridge)
		|| !TestNotNull(TEXT("Capture"), Capture))
	{
		Shutdown(GameInstance);
		return false;
	}
	TestTrue(TEXT("100-drone Fleet applied"), Fleet->ApplySnapshot(MakeFleet(100)));
	TArray<FString> Selected;
	for (int32 Index = 1; Index <= 8; ++Index)
	{
		Selected.Add(FString::Printf(TEXT("Drone%03d"), Index));
	}
	Bridge->ReplaceCommandDroneSelection(Selected);
	FString Error;
	TestTrue(TEXT("Capture begins from real command selection"),
		Capture->BeginCapture(MakePolicy(), Error));
	FRouteGpsCaptureSnapshot Snapshot;
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Fleet=100 selected=8 captures exactly 8"),
		Snapshot.Participants.Num(), 8);
	TestEqual(TEXT("Selection snapshot count is frozen"),
		Snapshot.SelectedDroneIDs.Num(), Selected.Num());
	for (const FString& DroneID : Selected)
	{
		TestTrue(TEXT("Selection snapshot contains selected participant"),
			Snapshot.SelectedDroneIDs.Contains(DroneID));
	}
	TestFalse(TEXT("Selection digest exists"), Snapshot.SelectionDigest.IsEmpty());

	const double BaseMonotonic = FPlatformTime::Seconds();
	constexpr int64 BaseUtcMs = 100000;
	for (int32 DroneIndex = 1; DroneIndex <= 8; ++DroneIndex)
	{
		const FString DroneID = FString::Printf(TEXT("Drone%03d"), DroneIndex);
		Capture->ObserveTelemetrySample(MakeObservation(
			*Capture, DroneID, DroneIndex, BaseUtcMs, BaseMonotonic, -0.0000001));
		if (DroneIndex == 1)
		{
			for (int32 Repeat = 0; Repeat < 20; ++Repeat)
			{
				Capture->ObserveTelemetrySample(MakeObservation(
					*Capture, DroneID, DroneIndex, BaseUtcMs,
					BaseMonotonic + Repeat * 0.001, -0.0000001));
			}
			Capture->GetCurrentSnapshot(Snapshot);
			TestEqual(TEXT("GPS #1 plus repeated telemetry remains one sample"),
				Snapshot.Participants[0].UniqueSampleCount, 1);
			TestEqual(TEXT("GPS #1 timestamp is preserved"),
				Snapshot.Participants[0].LastGpsSampleUtcMs, BaseUtcMs);
		}
		Capture->ObserveTelemetrySample(MakeObservation(
			*Capture, DroneID, DroneIndex, BaseUtcMs + 200,
			BaseMonotonic + 0.2));
		if (DroneIndex == 1)
		{
			Capture->GetCurrentSnapshot(Snapshot);
			TestEqual(TEXT("GPS #2 creates exactly the second unique sample"),
				Snapshot.Participants[0].UniqueSampleCount, 2);
			TestEqual(TEXT("GPS #2 timestamp advances to 1200-equivalent"),
				Snapshot.Participants[0].LastGpsSampleUtcMs, BaseUtcMs + 200);
		}
		Capture->ObserveTelemetrySample(MakeObservation(
			*Capture, DroneID, DroneIndex, BaseUtcMs + 400,
			BaseMonotonic + 0.4, 0.0000001));
	}
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Stable selected Fleet reaches Ready"),
		Snapshot.State, ERouteGpsCaptureState::Ready);
	for (const FRouteGpsCaptureParticipantResult& Participant : Snapshot.Participants)
	{
		TestEqual(TEXT("Repeated snapshots do not inflate unique count"),
			Participant.UniqueSampleCount, 3);
		TestEqual(TEXT("Participant status Ready"), Participant.Status,
			ERouteGpsCaptureParticipantStatus::Ready);
		TestEqual(TEXT("Stable window duration retained"),
			Participant.LastGpsSampleUtcMs - Participant.FirstGpsSampleUtcMs,
			static_cast<int64>(400));
	}
	Shutdown(GameInstance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteGpsCaptureInvalidationTest,
	"DroneGCS.Data.RouteGpsCapture.F1.Invalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteGpsCaptureInvalidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace RouteGpsCaptureTests;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteGpsCaptureInvalidation"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	UGCSUIBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
	URouteGpsCaptureSubsystem* Capture =
		GameInstance->GetSubsystem<URouteGpsCaptureSubsystem>();
	if (!Fleet || !Bridge || !Capture)
	{
		Shutdown(GameInstance);
		return false;
	}
	Fleet->ApplySnapshot(MakeFleet(3));
	const FRouteGpsCapturePolicy Policy = MakePolicy();
	const double BaseMonotonic = FPlatformTime::Seconds();
	FString Error;
	FRouteGpsCaptureSnapshot Snapshot;

	TestTrue(TEXT("Stale capture begins"), Capture->BeginCaptureForSelectionForTest(
		Policy, { TEXT("Drone001") }, Error));
	Capture->ObserveTelemetrySample(MakeObservation(
		*Capture, TEXT("Drone001"), 1, 200000, BaseMonotonic));
	Capture->EvaluateAtForTest(200600, BaseMonotonic + 0.6);
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Vehicle can stay online while GPS becomes stale"),
		Snapshot.Participants[0].FailureReason, ERouteGpsCaptureFailureReason::GpsStale);

	TestTrue(TEXT("Fix downgrade capture begins"), Capture->BeginCaptureForSelectionForTest(
		Policy, { TEXT("Drone001") }, Error));
	Capture->ObserveTelemetrySample(MakeObservation(
		*Capture, TEXT("Drone001"), 1, 300000, BaseMonotonic));
	Capture->ObserveTelemetrySample(MakeObservation(
		*Capture, TEXT("Drone001"), 1, 300100, BaseMonotonic + 0.1, 0.0, 5));
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Fix downgrade fails immediately"),
		Snapshot.Participants[0].FailureReason,
		ERouteGpsCaptureFailureReason::FixQualityInsufficient);

	TestTrue(TEXT("Endpoint capture begins"), Capture->BeginCaptureForSelectionForTest(
		Policy, { TEXT("Drone001") }, Error));
	Capture->ObserveTelemetrySample(MakeObservation(
		*Capture, TEXT("Drone001"), 1, 400000, BaseMonotonic,
		0.0, 6, FString(), TEXT("127.9.9.9:14550")));
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Endpoint identity change invalidates capture"), Snapshot.State,
		ERouteGpsCaptureState::Invalidated);
	TestEqual(TEXT("Endpoint failure is explicit"),
		Snapshot.Participants[0].FailureReason,
		ERouteGpsCaptureFailureReason::EndpointChanged);

	Bridge->ReplaceCommandDroneSelection({ TEXT("Drone001") });
	TestTrue(TEXT("Selection-bound capture begins"), Capture->BeginCapture(Policy, Error));
	Bridge->ReplaceCommandDroneSelection({ TEXT("Drone002") });
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Selection mutation invalidates capture"), Snapshot.State,
		ERouteGpsCaptureState::Invalidated);
	TestEqual(TEXT("Selection failure is explicit"),
		Snapshot.Participants[0].FailureReason,
		ERouteGpsCaptureFailureReason::SelectionInvalidated);

	TestTrue(TEXT("Offline capture begins"), Capture->BeginCaptureForSelectionForTest(
		Policy, { TEXT("Drone002") }, Error));
	TestTrue(TEXT("Offline Fleet update applies"),
		Fleet->ApplySnapshot(MakeFleet(3, TEXT("F1-FLEET"), 2)));
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Participant offline invalidates capture"),
		Snapshot.Participants[0].FailureReason, ERouteGpsCaptureFailureReason::Offline);

	TestTrue(TEXT("Fleet restored"), Fleet->ApplySnapshot(MakeFleet(3)));
	TestTrue(TEXT("Fleet-session capture begins"), Capture->BeginCaptureForSelectionForTest(
		Policy, { TEXT("Drone002") }, Error));
	TestTrue(TEXT("New Fleet session applies"),
		Fleet->ApplySnapshot(MakeFleet(3, TEXT("F1-NEW-FLEET"))));
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Fleet session change invalidates capture"),
		Snapshot.Participants[0].FailureReason,
		ERouteGpsCaptureFailureReason::FleetSessionChanged);
	Shutdown(GameInstance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteGpsCaptureTransactionIsolationTest,
	"DroneGCS.Data.RouteGpsCapture.F1.TransactionIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteGpsCaptureTransactionIsolationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace RouteGpsCaptureTests;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteGpsCaptureTransactionIsolation"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	URouteGpsCaptureSubsystem* Capture =
		GameInstance->GetSubsystem<URouteGpsCaptureSubsystem>();
	if (!Fleet || !Capture)
	{
		Shutdown(GameInstance);
		return false;
	}
	Fleet->ApplySnapshot(MakeFleet(1));
	FString Error;
	TestTrue(TEXT("Capture A begins"), Capture->BeginCaptureForSelectionForTest(
		MakePolicy(), { TEXT("Drone001") }, Error));
	FRouteGpsCaptureSnapshot SnapshotA;
	Capture->GetCurrentSnapshot(SnapshotA);
	TestTrue(TEXT("Capture B begins"), Capture->BeginCaptureForSelectionForTest(
		MakePolicy(), { TEXT("Drone001") }, Error));
	FRouteGpsCaptureSnapshot SnapshotB;
	Capture->GetCurrentSnapshot(SnapshotB);
	TestNotEqual(TEXT("Capture IDs are unique"),
		SnapshotA.CaptureRequestID, SnapshotB.CaptureRequestID);
	TestTrue(TEXT("Capture revision advances"),
		SnapshotB.CaptureRevision > SnapshotA.CaptureRevision);
	const double BaseMonotonic = FPlatformTime::Seconds();
	Capture->ObserveTelemetrySample(MakeObservation(
		*Capture, TEXT("Drone001"), 1, 500000, BaseMonotonic,
		0.0, 6, SnapshotA.CaptureRequestID));
	FRouteGpsCaptureSnapshot AfterLateA;
	Capture->GetCurrentSnapshot(AfterLateA);
	TestEqual(TEXT("Late A cannot replace B"),
		AfterLateA.CaptureRequestID, SnapshotB.CaptureRequestID);
	TestEqual(TEXT("Late A cannot add a sample to B"),
		AfterLateA.Participants[0].UniqueSampleCount, 0);
	for (int32 Sample = 0; Sample < 3; ++Sample)
	{
		Capture->ObserveTelemetrySample(MakeObservation(
			*Capture, TEXT("Drone001"), 1, 500000 + Sample * 200,
			BaseMonotonic + Sample * 0.2, (Sample - 1) * 0.0000001,
			6, SnapshotB.CaptureRequestID));
	}
	Capture->GetCurrentSnapshot(SnapshotB);
	TestEqual(TEXT("B reaches Ready only from B-tagged samples"), SnapshotB.State,
		ERouteGpsCaptureState::Ready);
	Capture->EvaluateAtForTest(501500, BaseMonotonic + 1.5);
	Capture->GetCurrentSnapshot(SnapshotB);
	TestEqual(TEXT("Ready capture is an immutable result until identity authority changes"),
		SnapshotB.State, ERouteGpsCaptureState::Ready);
	Shutdown(GameInstance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteGpsCaptureRobustnessTest,
	"DroneGCS.Data.RouteGpsCapture.F1.Robustness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteGpsCaptureRobustnessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace RouteGpsCaptureTests;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteGpsCaptureRobustness"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	URouteGpsCaptureSubsystem* Capture =
		GameInstance->GetSubsystem<URouteGpsCaptureSubsystem>();
	if (!Fleet || !Capture)
	{
		Shutdown(GameInstance);
		return false;
	}
	Fleet->ApplySnapshot(MakeFleet(1));
	FString Error;
	const double BaseMonotonic = FPlatformTime::Seconds();
	TestTrue(TEXT("Jump capture begins"), Capture->BeginCaptureForSelectionForTest(
		MakePolicy(), { TEXT("Drone001") }, Error));
	Capture->ObserveTelemetrySample(MakeObservation(
		*Capture, TEXT("Drone001"), 1, 600000, BaseMonotonic));
	Capture->ObserveTelemetrySample(MakeObservation(
		*Capture, TEXT("Drone001"), 1, 600200, BaseMonotonic + 0.2, 0.0001));
	FRouteGpsCaptureSnapshot Snapshot;
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Single large jump fails deterministically"),
		Snapshot.Participants[0].FailureReason,
		ERouteGpsCaptureFailureReason::UnstablePosition);

	FRouteGpsCapturePolicy DriftPolicy = MakePolicy();
	DriftPolicy.MaxJumpDistanceMeters = 5.0;
	DriftPolicy.MaxMedianResidualMeters = 0.2;
	DriftPolicy.MaxSampleResidualMeters = 0.3;
	TestTrue(TEXT("Drift capture begins"), Capture->BeginCaptureForSelectionForTest(
		DriftPolicy, { TEXT("Drone001") }, Error));
	for (int32 Sample = 0; Sample < 3; ++Sample)
	{
		Capture->ObserveTelemetrySample(MakeObservation(
			*Capture, TEXT("Drone001"), 1, 700000 + Sample * 200,
			BaseMonotonic + 1.0 + Sample * 0.2, Sample * 0.0000045));
	}
	Capture->GetCurrentSnapshot(Snapshot);
	TestEqual(TEXT("Continuous drift fails metric stability gates"),
		Snapshot.Participants[0].FailureReason,
		ERouteGpsCaptureFailureReason::UnstablePosition);

	const auto RunDeterministicStableCapture = [Capture, BaseMonotonic, &Error]()
	{
		Capture->BeginCaptureForSelectionForTest(
			MakePolicy(), { TEXT("Drone001") }, Error);
		for (int32 Sample = 0; Sample < 3; ++Sample)
		{
			Capture->ObserveTelemetrySample(MakeObservation(
				*Capture, TEXT("Drone001"), 1, 800000 + Sample * 200,
				BaseMonotonic + 2.0 + Sample * 0.2,
				(Sample - 1) * 0.0000001));
		}
		FRouteGpsCaptureSnapshot StableSnapshot;
		Capture->GetCurrentSnapshot(StableSnapshot);
		return StableSnapshot.Participants[0];
	};
	const FRouteGpsCaptureParticipantResult First = RunDeterministicStableCapture();
	const FRouteGpsCaptureParticipantResult Second = RunDeterministicStableCapture();
	TestEqual(TEXT("Deterministic latitude estimate"),
		First.EstimatedLatitude, Second.EstimatedLatitude);
	TestEqual(TEXT("Deterministic longitude estimate"),
		First.EstimatedLongitude, Second.EstimatedLongitude);
	TestEqual(TEXT("Deterministic robust spread"),
		First.RobustSpreadMeters, Second.RobustSpreadMeters);
	Shutdown(GameInstance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteGpsCaptureExplicitParticipantTest,
	"DroneGCS.Data.RouteGpsCapture.F1.ExplicitParticipantAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteGpsCaptureExplicitParticipantTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace RouteGpsCaptureTests;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteGpsCaptureExplicitParticipant"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	UGCSUIBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>();
	URouteGpsCaptureSubsystem* Capture =
		GameInstance->GetSubsystem<URouteGpsCaptureSubsystem>();
	if (!Fleet || !Bridge || !Capture)
	{
		Shutdown(GameInstance);
		return false;
	}
	Fleet->ApplySnapshot(MakeFleet(3));
	Bridge->ReplaceCommandDroneSelection({TEXT("Drone001"), TEXT("Drone002")});
	const TArray<FString> OriginalSelection = Bridge->GetSelectedCommandDroneIDs();
	FString Error;
	TestTrue(TEXT("Explicit Drone003 capture begins"),
		Capture->BeginCaptureForParticipants(
			MakePolicy(), {TEXT("Drone003")}, Error));
	FRouteGpsCaptureSnapshot Explicit;
	Capture->GetCurrentSnapshot(Explicit);
	TestFalse(TEXT("Explicit capture is not Command Selection bound"),
		Explicit.bCommandSelectionBound);
	TestEqual(TEXT("Explicit capture has exactly one requested participant"),
		Explicit.SelectedDroneIDs, TArray<FString>{TEXT("Drone003")});
	TestEqual(TEXT("Starting explicit capture preserves Command Selection"),
		Bridge->GetSelectedCommandDroneIDs(), OriginalSelection);

	Bridge->ReplaceCommandDroneSelection({TEXT("Drone001")});
	Capture->GetCurrentSnapshot(Explicit);
	TestEqual(TEXT("Command Selection changes do not invalidate explicit capture"),
		Explicit.State, ERouteGpsCaptureState::Capturing);
	const double BaseMonotonic = FPlatformTime::Seconds();
	for (int32 Sample = 0; Sample < 3; ++Sample)
	{
		Capture->ObserveTelemetrySample(MakeObservation(
			*Capture, TEXT("Drone003"), 3, 800000 + Sample * 200,
			BaseMonotonic + Sample * 0.2, (Sample - 1) * 0.0000001,
			6, Explicit.CaptureRequestID));
	}
	Capture->GetCurrentSnapshot(Explicit);
	TestEqual(TEXT("Explicit participant uses the F.1 stable estimator"),
		Explicit.State, ERouteGpsCaptureState::Ready);
	TestEqual(TEXT("Explicit result remains bound to Drone003"),
		Explicit.Participants[0].DroneID, FString(TEXT("Drone003")));
	TestEqual(TEXT("Command Selection remains operator-owned after Ready"),
		Bridge->GetSelectedCommandDroneIDs(), TArray<FString>{TEXT("Drone001")});
	Shutdown(GameInstance);
	return true;
}

#endif
