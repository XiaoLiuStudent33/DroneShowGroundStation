#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DSPGCSFrozenAssignmentProtocol.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSFrozenAssignmentSharedContractTest,
	"DroneGCS.Data.DSPGCSFrozenAssignment.SharedContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSFrozenAssignmentSharedContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	dspgcs::FrozenAssignmentPayloadDTO Payload;
	Payload.sourceDatasetDigest.fill(0x5a);
	Payload.normalizedShowOriginLatitudeE7 = 301230000;
	Payload.normalizedShowOriginLongitudeE7 = 1204560000;
	Payload.identity.fleetSessionId = "UE-SHARED-COMPILE-FLEET";
	Payload.identity.assignmentAuthoritySessionId =
		"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
	Payload.identity.assignmentRevision = 1;
	Payload.fleetRevisionAtCapture = 2;
	Payload.routeDatasetGeneration = 3;
	Payload.routeSlotCount = 1;
	Payload.matchedCount = 1;
	Payload.mappings.push_back({ "Drone000001", 0 });

	std::string Error;
	std::vector<std::uint8_t> CanonicalBytes;
	TestTrue(TEXT("UE compiles and serializes the Shared canonical payload"),
		dspgcs::SerializeCanonicalFrozenAssignmentPayload(
			Payload, CanonicalBytes, Error));

	dspgcs::FrozenAssignmentDigest Digest{};
	TestTrue(TEXT("UE computes the Shared SHA-256 digest"),
		dspgcs::ComputeFrozenAssignmentSha256(CanonicalBytes, Digest, Error));

	dspgcs::MessageEnvelope Envelope;
	Envelope.messageType = dspgcs::FrozenAssignmentMessageTypes::Chunk;
	Envelope.sourceId = "UE-PRODUCTION";
	Envelope.sessionId = "UE-SHARED-COMPILE";
	Envelope.correlationId = "TX-UE-SHARED-COMPILE";
	dspgcs::FrozenAssignmentChunkPlan Plan;
	TestTrue(TEXT("UE compiles and plans Shared Frozen chunks"),
		dspgcs::PlanFrozenAssignmentChunks(
			CanonicalBytes,
			dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes,
			Envelope,
			"TX-UE-SHARED-COMPILE",
			Plan,
			Error));
	TestEqual(TEXT("Small canonical payload uses one chunk"),
		static_cast<int32>(Plan.chunks.size()), 1);
	return true;
}

#endif
