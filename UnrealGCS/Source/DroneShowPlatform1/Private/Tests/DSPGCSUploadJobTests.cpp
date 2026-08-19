#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DSPGCSUploadJobProtocol.h"
#include "DSPGCSUploadJobStateMirror.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactReadLease.h"
#include "RouteArtifactWorkspaceManager.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
	std::string Id(const char Value) { return std::string(32, Value); }

	dspgcs::UploadArtifactIdentityDTO Identity()
	{
		dspgcs::UploadArtifactIdentityDTO Result;
		Result.artifactSetId = std::string(64, 'a');
		Result.fleetSessionId = "fleet";
		Result.assignmentAuthoritySessionId = "authority";
		Result.assignmentRevision = 1;
		return Result;
	}

	dspgcs::UploadJobSnapshotDTO Snapshot()
	{
		dspgcs::UploadJobSnapshotDTO Result;
		Result.uploadJobId = Id('1');
		Result.snapshotToken = Id('2');
		Result.artifactIdentity = Identity();
		Result.artifactGateState = dspgcs::UploadArtifactGateState::Validated;
		Result.jobState = dspgcs::UploadJobState::Running;
		Result.jobRevision = 3;
		Result.snapshotSequence = 4;
		Result.routeDeltaSequence = 7;
		Result.matchedCount = 2;
		Result.waitingCount = 2;
		Result.totalBytes = 200;
		Result.effectiveMaxConcurrentUploads = 2;
		return Result;
	}

	dspgcs::UploadRouteStateDTO Route(const int32 Slot)
	{
		dspgcs::UploadRouteStateDTO Result;
		Result.routeSlotIndex = Slot;
		Result.droneId = "drone-" + std::to_string(Slot);
		Result.totalBytes = 100;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDSPGCSUploadJobMirrorReliabilityTest,
	"DroneGCS.Data.DSPGCSUploadJob.MirrorReliability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDSPGCSUploadJobMirrorReliabilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FDSPGCSUploadJobStateMirror Mirror;
	auto Baseline = Snapshot();
	std::vector<std::uint8_t> Bytes;
	std::string Error;
	TestTrue(TEXT("Aggregate encodes"),
		dspgcs::EncodeUploadJobSnapshot(Baseline, Bytes, Error));
	dspgcs::UploadJobSnapshotDTO Decoded;
	TestTrue(TEXT("Aggregate decodes"),
		dspgcs::DecodeUploadJobSnapshot(Bytes, Decoded, Error));
	TestTrue(TEXT("Mirror accepts authoritative snapshot"), Mirror.ApplySnapshot(Decoded));

	dspgcs::UploadJobRouteStatePageDTO Page;
	Page.clientRequestId = Id('3');
	Page.uploadJobId = Baseline.uploadJobId;
	Page.snapshotToken = Baseline.snapshotToken;
	Page.jobRevision = Baseline.jobRevision;
	Page.totalRouteCount = 2;
	Page.pageStart = 0;
	Page.nextPageStart = 2;
	Page.routes = { Route(0), Route(1) };
	TestTrue(TEXT("Consistent page applies"), Mirror.ApplyRoutePage(Page));
	TestTrue(TEXT("Page completes route mirror"),
		Mirror.IsRoutePageComplete() && Mirror.GetRouteStateCount() == 2);
	TArray<int32> SortedRouteSlots;
	Mirror.CopySortedRouteSlots(SortedRouteSlots);
	TestEqual(TEXT("Sorted route slot count"), SortedRouteSlots.Num(), 2);
	if (SortedRouteSlots.Num() == 2)
	{
		TestEqual(TEXT("First sorted route slot"), SortedRouteSlots[0], 0);
		TestEqual(TEXT("Second sorted route slot"), SortedRouteSlots[1], 1);
	}

	dspgcs::UploadJobRouteStateDeltaDTO Delta;
	Delta.uploadJobId = Baseline.uploadJobId;
	Delta.jobRevision = 4;
	Delta.deltaSequence = 8;
	auto Changed = Route(0);
	Changed.state = dspgcs::UploadRouteState::Uploading;
	Changed.bytesCompleted = 10;
	Delta.routes.push_back(Changed);
	bool bDuplicate = false;
	TestTrue(TEXT("Next ordered absolute Delta applies"),
		Mirror.ApplyRouteDelta(Delta, bDuplicate));
	TestTrue(TEXT("Duplicate Delta is harmless"),
		Mirror.ApplyRouteDelta(Delta, bDuplicate) && bDuplicate);

	auto Gap = Delta;
	Gap.deltaSequence = 10;
	Gap.jobRevision = 5;
	TestFalse(TEXT("Delta gap is rejected"), Mirror.ApplyRouteDelta(Gap, bDuplicate));
	TestTrue(TEXT("Delta gap requests recovery"), Mirror.IsRecoveryRequired());

	Mirror.ApplySnapshot(Baseline);
	auto ReorderedPage = Page;
	ReorderedPage.pageStart = 1;
	TestFalse(TEXT("Reordered page rejected"), Mirror.ApplyRoutePage(ReorderedPage));
	Mirror.ApplySnapshot(Baseline);
	Mirror.MarkTransportReconnected();
	TestTrue(TEXT("Reconnect requests snapshot"), Mirror.IsRecoveryRequired());
	TestTrue(TEXT("Lost snapshot leaves recovery pending"), Mirror.IsRecoveryRequired());
	TestTrue(TEXT("Retried snapshot recovers mirror"), Mirror.ApplySnapshot(Baseline));
	TestFalse(TEXT("Recovered snapshot clears recovery"), Mirror.IsRecoveryRequired());
	Mirror.MarkPageOpened();
	TestTrue(TEXT("Page reopen requests snapshot"), Mirror.IsRecoveryRequired());

	dspgcs::UploadJobCommandAckDTO Ack;
	Ack.clientCommandId = Id('4');
	Ack.uploadJobId = Baseline.uploadJobId;
	Ack.commandType = dspgcs::UploadJobCommandType::Pause;
	Ack.status = dspgcs::UploadJobCommandAckStatus::Accepted;
	Mirror.SetPendingCommandID(Ack.clientCommandId);
	TestTrue(TEXT("Matching ACK completes pending command"), Mirror.ApplyCommandAck(Ack));
	TestFalse(TEXT("Duplicate ACK ignored"), Mirror.ApplyCommandAck(Ack));
	Ack.clientCommandId = Id('5');
	TestFalse(TEXT("Late unrelated ACK ignored"), Mirror.ApplyCommandAck(Ack));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactUploadLeasePromotionGateTest,
	"DroneGCS.Data.RouteArtifact.UploadLeasePromotionGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactUploadLeasePromotionGateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Root = FPaths::Combine(
		FPlatformProcess::UserTempDir(),
		TEXT("DroneGCS-2GB-Lease-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Current")), true);
	std::atomic<bool> bHeld{ false };
	std::atomic<bool> bRelease{ false };
	std::atomic<bool> bOwnerExited{ false };
	std::atomic<bool> bOwnerReleaseTimedOut{ false };
	std::atomic<bool> bAcquireFailed{ false };
	constexpr auto WaitTimeout = std::chrono::seconds(5);
	std::thread Owner([&]
	{
		dronegcs::route_artifact::ArtifactReadLease Lease;
		bool bRecovered = false;
		std::string Error;
		if (Lease.TryAcquire(std::wstring(*Root), bRecovered, Error) ==
			dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
		{
			bHeld.store(true);
			const auto ReleaseDeadline = std::chrono::steady_clock::now() + WaitTimeout;
			while (!bRelease.load() &&
				std::chrono::steady_clock::now() < ReleaseDeadline)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			bOwnerReleaseTimedOut.store(!bRelease.load());
		}
		else
		{
			bAcquireFailed.store(true);
		}
		bOwnerExited.store(true);
	});
	const auto HeldDeadline = std::chrono::steady_clock::now() + WaitTimeout;
	while (!bHeld.load() && !bOwnerExited.load() &&
		std::chrono::steady_clock::now() < HeldDeadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	if (!bHeld.load())
	{
		bRelease.store(true);
		if (Owner.joinable()) Owner.join();
		AddError(bAcquireFailed.load()
			? TEXT("Upload lease owner failed to acquire the test mutex.")
			: TEXT("Timed out waiting for the Upload lease owner."));
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	bool bRecovered = false;
	FString GateError;
	TestFalse(TEXT("Promotion is Busy while Upload lease is held"),
		FRouteArtifactWorkspaceManager::TryAcquireCurrentPromotionGateForTest(
			Root, bRecovered, GateError));
	TestTrue(TEXT("Busy result is explicit"),
		GateError.Contains(TEXT("ArtifactCurrentLeaseBusy")));
	bRelease.store(true);
	if (Owner.joinable()) Owner.join();
	if (bOwnerReleaseTimedOut.load())
	{
		AddError(TEXT("Upload lease owner timed out waiting for release."));
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	TestTrue(TEXT("Promotion gate opens after lease release"),
		FRouteArtifactWorkspaceManager::TryAcquireCurrentPromotionGateForTest(
			Root, bRecovered, GateError));
	TArray<FString> Entries;
	IFileManager::Get().FindFiles(
		Entries, *FPaths::Combine(Root, TEXT("*")), false, true);
	TestEqual(TEXT("Lease creates no historical Artifact archive"), Entries.Num(), 1);
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif
