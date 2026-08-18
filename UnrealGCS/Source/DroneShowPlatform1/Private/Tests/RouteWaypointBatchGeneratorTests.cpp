#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceManager.h"
#include "RouteWaypointBatchGenerator.h"

#include <limits>

namespace
{
	FRouteArtifactDigest256 BatchHash(FAutomationTestBase& Test, const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FRouteArtifactDigest256 Digest;
		FString Error;
		Test.TestTrue(TEXT("Batch fixture SHA-256"), FRouteArtifactSha256::HashBytes(
			Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest, Error));
		return Digest;
	}

	FRouteArtifactSourceIdentity BatchSource(
		FAutomationTestBase& Test,
		const FString& Label = TEXT("batch-source"))
	{
		FRouteArtifactSourceIdentity Source;
		FString Error;
		Test.TestTrue(TEXT("Batch source identity"), FRouteArtifactSourceIdentity::Create(
			BatchHash(Test, Label), 43210, Label + TEXT(".npz"), Source, Error));
		return Source;
	}

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> MakeBatchData(
		FAutomationTestBase& Test,
		const int32 FrameCount,
		const int32 RouteCount,
		const FString& SourceLabel = TEXT("batch-source"))
	{
		TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
			MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>();
		Data->FrameCount = FrameCount;
		Data->DroneCount = RouteCount;
		Data->SourceIdentity = BatchSource(Test, SourceLabel);
		Data->ShowOriginLatitude = 31.932471;
		Data->ShowOriginLongitude = 118.638531;
		Data->ShowOriginAltitude = 50.0;
		Data->bShowOriginSet = true;
		const int64 Samples = static_cast<int64>(FrameCount) * RouteCount;
		Data->PositionsENU.SetNumUninitialized(static_cast<int32>(Samples));
		Data->Colors.SetNumUninitialized(static_cast<int32>(Samples));
		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			for (int32 Route = 0; Route < RouteCount; ++Route)
			{
				const int32 Index = Frame * RouteCount + Route;
				Data->PositionsENU[Index] = FVector3f(
					Route * 0.1f, Frame * 0.2f, (Frame - Route) * 0.01f);
				Data->Colors[Index] = FFloat16Color(
					FLinearColor(0.25f, 0.5f, 0.75f, 1.0f));
			}
		}
		return Data;
	}

	FRouteWaypointGenerationSnapshot MakeSnapshot(
		FAutomationTestBase& Test,
		const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe>& Data,
		const int64 Generation = 3)
	{
		FRouteWaypointGenerationSnapshot Snapshot;
		Snapshot.RouteData = Data;
		Snapshot.SourceIdentity = Data->SourceIdentity;
		Snapshot.RouteDatasetGeneration = Generation;
		FRouteWaypointGeneratorError Error;
		Test.TestTrue(TEXT("Batch generator context"), RouteWaypointGenerator::BuildContext(
			Data->ShowOriginLatitude, Data->ShowOriginLongitude,
			Snapshot.GeneratorContext, Error));
		Snapshot.WaypointGeneratorVersion = Snapshot.GeneratorContext.WaypointGeneratorVersion;
		Snapshot.GeneratorInputDigest = Snapshot.GeneratorContext.GeneratorInputDigest;
		Snapshot.FrameCount = Data->FrameCount;
		Snapshot.RouteCount = Data->DroneCount;
		return Snapshot;
	}

	FRouteArtifactBuildIdentity MakeBatchBuild(
		FAutomationTestBase& Test,
		const FRouteWaypointGenerationSnapshot& Snapshot,
		const FString& Label)
	{
		FRouteArtifactBuildIdentity Build;
		Build.AssignmentBinding.FleetSessionID = TEXT("batch-fleet-session");
		Build.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("batch-authority-session");
		Build.AssignmentBinding.AssignmentRevision = 7;
		Build.AssignmentBinding.AssignmentDigest = BatchHash(Test, TEXT("batch-assignment-") + Label);
		Build.SourceDatasetDigest = Snapshot.SourceIdentity.SourceDatasetDigest;
		Build.WaypointGeneratorVersion = Snapshot.WaypointGeneratorVersion;
		Build.GeneratorInputDigest = Snapshot.GeneratorInputDigest;
		return Build;
	}

	FRouteArtifactWorkspaceAuthorityGate MakeBatchGate(
		const FRouteArtifactBuildIdentity& Build,
		const FRouteWaypointGenerationSnapshot& Snapshot)
	{
		FRouteArtifactWorkspaceAuthorityGate Gate;
		Gate.bHasFrozenSnapshot = true;
		Gate.FrozenFleetSessionID = Build.AssignmentBinding.FleetSessionID;
		Gate.FrozenAssignmentAuthoritySessionID =
			Build.AssignmentBinding.AssignmentAuthoritySessionID;
		Gate.FrozenAssignmentRevision = Build.AssignmentBinding.AssignmentRevision;
		Gate.bConsoleAccepted = true;
		Gate.ConsoleAcceptedBinding = Build.AssignmentBinding;
		Gate.bSourceExists = true;
		Gate.ActiveSourceIdentity = Snapshot.SourceIdentity;
		Gate.RouteSlotCount = Snapshot.RouteCount;
		return Gate;
	}

	FRouteArtifactManifest MakeBatchDraft(
		const FRouteArtifactBuildIdentity& Build,
		const FRouteWaypointGenerationSnapshot& Snapshot,
		const FString& ArtifactSetID,
		const int32 MatchedCount,
		const int64 CreatedAtUtc = 1700000000123)
	{
		FRouteArtifactManifest Draft;
		Draft.ArtifactSetID = ArtifactSetID;
		Draft.CreatedAtUtc = CreatedAtUtc;
		Draft.AssignmentBinding = Build.AssignmentBinding;
		Draft.FleetRevisionAtCapture = 11;
		Draft.RouteDatasetGeneration = Snapshot.RouteDatasetGeneration;
		Draft.CompletionMode = MatchedCount == Snapshot.RouteCount
			? ERouteAssignmentCompletionMode::Full
			: ERouteAssignmentCompletionMode::Degraded;
		Draft.FreezePolicy = MatchedCount == Snapshot.RouteCount
			? ERouteAssignmentFreezePolicy::RequireFull
			: ERouteAssignmentFreezePolicy::AllowDegraded;
		Draft.MatchedCount = MatchedCount;
		Draft.VacantCount = Snapshot.RouteCount - MatchedCount;
		Draft.SourceIdentity = Snapshot.SourceIdentity;
		Draft.FrameCount = Snapshot.FrameCount;
		Draft.RouteSlotCount = Snapshot.RouteCount;
		Draft.WaypointGeneratorVersion = Snapshot.WaypointGeneratorVersion;
		Draft.GeneratorInputDigest = Snapshot.GeneratorInputDigest;
		for (int32 Slot = 0; Slot < MatchedCount; ++Slot)
		{
			FRouteArtifactMatchedMapping& Mapping = Draft.MatchedMapping.AddDefaulted_GetRef();
			Mapping.DroneID = FString::Printf(TEXT("drone-%06d"), Slot);
			Mapping.RouteSlotIndex = Slot;
		}
		return Draft;
	}

	FString BatchTempRoot(const FString& Label)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("RouteWaypointBatchTests"),
			Label + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	struct FBatchArtifactImage
	{
		FString ArtifactSetID;
		FString ArtifactSetDigest;
		TArray<uint8> ManifestBytes;
		TArray<TArray<uint8>> RouteBytes;
		FRouteWaypointBatchProgress Progress;
		int64 FinalizeValidationReads = 0;
	};

	bool RunSuccessfulBatch(
		FAutomationTestBase& Test,
		const int32 FrameCount,
		const int32 RouteCount,
		const int32 WorkerCount,
		const int32 MatchedCount,
		const FString& IdentityLabel,
		FBatchArtifactImage& OutImage)
	{
		const FString Root = BatchTempRoot(TEXT("success"));
		const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
			MakeBatchData(Test, FrameCount, RouteCount);
		const FRouteWaypointGenerationSnapshot Snapshot = MakeSnapshot(Test, Data);
		const FRouteArtifactBuildIdentity Build = MakeBatchBuild(Test, Snapshot, IdentityLabel);
		const FRouteArtifactWorkspaceAuthorityGate Gate = MakeBatchGate(Build, Snapshot);
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		if (!Manager.SetConfiguredWorkspaceRoot(Root, Error))
		{
			Test.AddError(Error);
			return false;
		}
		FRouteArtifactGenerationHandle Handle;
		if (!Manager.BeginGeneration(Build, Gate, Handle, Error))
		{
			Test.AddError(Error);
			return false;
		}

		FRouteWaypointBatchRequest Request;
		Request.Snapshot = Snapshot;
		Request.WorkspaceManager = &Manager;
		Request.AttemptID = Handle.AttemptID;
		Request.ArtifactSetID = Handle.ArtifactSetID;
		Request.WorkerCountOverride = WorkerCount;
		FRouteWaypointBatchGenerator Batch;
		if (!Batch.Start(Request, Error))
		{
			Test.AddError(Error);
			return false;
		}
		Batch.Wait();
		OutImage.Progress = Batch.GetProgress();
		if (OutImage.Progress.GenerationState != ERouteWaypointBatchState::WorkersSucceeded)
		{
			Test.AddError(TEXT("Synthetic batch workers did not all succeed."));
			return false;
		}
		FRouteArtifactManifest Manifest = MakeBatchDraft(
			Build, Snapshot, Handle.ArtifactSetID, MatchedCount);
		if (!Manager.FinalizeGeneratedRoutes(
			Manifest, Gate, Snapshot.MakeSourceValidationContext(), Error))
		{
			Test.AddError(Error);
			return false;
		}
		OutImage.FinalizeValidationReads = Manager.GetTestFinalizeRouteValidationReadCount();
		OutImage.ArtifactSetID = Manifest.ArtifactSetID;
		OutImage.ArtifactSetDigest = Manifest.ArtifactSetDigest.ToLowerHex();
		if (!RouteArtifactManifest::SerializeCanonicalManifest(
			Manifest, OutImage.ManifestBytes, Error))
		{
			Test.AddError(Error);
			return false;
		}
		OutImage.RouteBytes.SetNum(RouteCount);
		for (const FRouteArtifactRecord& Record : Manifest.Routes)
		{
			const FString Path = FPaths::Combine(
				Manager.GetCurrentInfo().CurrentRoot, Record.RelativePath);
			if (!FFileHelper::LoadFileToArray(
				OutImage.RouteBytes[Record.RouteSlotIndex], *Path))
			{
				Test.AddError(TEXT("Could not read finalized route fixture."));
				return false;
			}
		}
		Manager.Shutdown();
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return true;
	}

	bool BeginBatchFixture(
		FAutomationTestBase& Test,
		FRouteArtifactWorkspaceManager& Manager,
		const FString& Root,
		const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe>& Data,
		const FString& Label,
		const int32 WorkerCount,
		FRouteArtifactBuildIdentity& OutBuild,
		FRouteArtifactWorkspaceAuthorityGate& OutGate,
		FRouteArtifactGenerationHandle& OutHandle,
		FRouteWaypointBatchGenerator& OutBatch,
		const int32 FailWriterSlot = INDEX_NONE,
		const int32 DelayMilliseconds = 0)
	{
		FString Error;
		if (Manager.GetConfiguredWorkspaceRoot().IsEmpty()
			&& !Manager.SetConfiguredWorkspaceRoot(Root, Error))
		{
			Test.AddError(Error);
			return false;
		}
		const FRouteWaypointGenerationSnapshot Snapshot = MakeSnapshot(Test, Data);
		OutBuild = MakeBatchBuild(Test, Snapshot, Label);
		OutGate = MakeBatchGate(OutBuild, Snapshot);
		if (!Manager.BeginGeneration(OutBuild, OutGate, OutHandle, Error))
		{
			Test.AddError(Error);
			return false;
		}
		FRouteWaypointBatchRequest Request;
		Request.Snapshot = Snapshot;
		Request.WorkspaceManager = &Manager;
		Request.AttemptID = OutHandle.AttemptID;
		Request.ArtifactSetID = OutHandle.ArtifactSetID;
		Request.WorkerCountOverride = WorkerCount;
		Request.TestFailWriterRouteSlot = FailWriterSlot;
		Request.TestDelayPerRouteMilliseconds = DelayMilliseconds;
		if (!OutBatch.Start(Request, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointBatchDeterminismTests,
	"DroneGCS.RouteWaypointBatch.2DC2A.DeterminismAndDegraded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointBatchDeterminismTests::RunTest(const FString& Parameters)
{
	for (const int32 RouteCount : {3, 32})
	{
		FBatchArtifactImage Baseline;
		TestTrue(TEXT("Worker=1 batch"), RunSuccessfulBatch(
			*this, 3, RouteCount, 1, RouteCount, TEXT("determinism"), Baseline));
		for (const int32 Workers : {2, FMath::Min(4, RouteCount)})
		{
			FBatchArtifactImage Concurrent;
			TestTrue(TEXT("Concurrent batch"), RunSuccessfulBatch(
				*this, 3, RouteCount, Workers, RouteCount,
				TEXT("determinism"), Concurrent));
			TestEqual(TEXT("ArtifactSetID deterministic"),
				Concurrent.ArtifactSetID, Baseline.ArtifactSetID);
			TestEqual(TEXT("ArtifactSetDigest deterministic"),
				Concurrent.ArtifactSetDigest, Baseline.ArtifactSetDigest);
			TestEqual(TEXT("Manifest bytes deterministic"),
				Concurrent.ManifestBytes, Baseline.ManifestBytes);
			TestEqual(TEXT("Every route byte stream deterministic"),
				Concurrent.RouteBytes, Baseline.RouteBytes);
			TestTrue(TEXT("Peak workers bounded"),
				Concurrent.Progress.PeakActiveWorkers <= Workers);
			TestTrue(TEXT("Peak files bounded"),
				Concurrent.Progress.PeakOpenRouteFiles <= Workers);
			TestEqual(TEXT("One authoritative finalize validation read per route"),
				Concurrent.FinalizeValidationReads, static_cast<int64>(RouteCount));
		}
	}

	FBatchArtifactImage Degraded;
	TestTrue(TEXT("Degraded full Route domain generation"), RunSuccessfulBatch(
		*this, 3, 5, 2, 3, TEXT("degraded"), Degraded));
	TestEqual(TEXT("Degraded still has five route files"), Degraded.RouteBytes.Num(), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointBatchFailureCancelTests,
	"DroneGCS.RouteWaypointBatch.2DC2A.FailureCancelAndStaleGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointBatchFailureCancelTests::RunTest(const FString& Parameters)
{
	AddExpectedError(
		TEXT("Artifact generation failed."),
		EAutomationExpectedErrorFlags::Contains,
		4);
	const FString Root = BatchTempRoot(TEXT("failure"));
	FRouteArtifactWorkspaceManager Manager;
	FString Error;
	TestTrue(TEXT("Failure Workspace root"), Manager.SetConfiguredWorkspaceRoot(Root, Error));
	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> BaselineData =
		MakeBatchData(*this, 2, 2, TEXT("baseline-current"));
	const FRouteWaypointGenerationSnapshot BaselineSnapshot =
		MakeSnapshot(*this, BaselineData);
	FRouteArtifactBuildIdentity BaselineBuild;
	FRouteArtifactWorkspaceAuthorityGate BaselineGate;
	FRouteArtifactGenerationHandle BaselineHandle;
	FRouteWaypointBatchGenerator BaselineBatch;
	TestTrue(TEXT("Begin baseline Current"), BeginBatchFixture(
		*this, Manager, Root, BaselineData, TEXT("baseline-current"), 1,
		BaselineBuild, BaselineGate, BaselineHandle, BaselineBatch));
	BaselineBatch.Wait();
	FRouteArtifactManifest BaselineDraft = MakeBatchDraft(
		BaselineBuild, BaselineSnapshot, BaselineHandle.ArtifactSetID, 2);
	TestTrue(TEXT("Finalize baseline Current"),
		Manager.FinalizeGeneratedRoutes(
			BaselineDraft, BaselineGate,
			BaselineSnapshot.MakeSourceValidationContext(), Error));
	const FString BaselineCurrentSetID = Manager.GetCurrentInfo().ArtifactSetID;

	auto ExpectRouteFailure = [this, &Manager, &Root, &BaselineCurrentSetID](
		const FString& Label,
		const TFunction<void(FRoutePreviewData&)>& Mutate,
		const int32 WriterFailSlot,
		const int32 ExpectedFailureSlot)
	{
		TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
			MakeBatchData(*this, 128, 16, TEXT("failure-source"));
		Mutate(*Data);
		FRouteArtifactBuildIdentity Build;
		FRouteArtifactWorkspaceAuthorityGate Gate;
		FRouteArtifactGenerationHandle Handle;
		FRouteWaypointBatchGenerator Batch;
		if (!BeginBatchFixture(*this, Manager, Root, Data, Label, 2,
			Build, Gate, Handle, Batch, WriterFailSlot))
		{
			return;
		}
		Batch.Wait();
		const FRouteWaypointBatchProgress Progress = Batch.GetProgress();
		TestEqual(TEXT("Route failure fails whole batch"), Progress.GenerationState,
			ERouteWaypointBatchState::Failed);
		TestEqual(TEXT("Root failure route preserved"),
			Progress.FirstFailureRouteSlotIndex, ExpectedFailureSlot);
		TestTrue(TEXT("Root failure is not Cancelled"),
			Progress.FirstFailureErrorCode != ERouteWaypointGeneratorErrorCode::Cancelled);
		if (ExpectedFailureSlot < 15)
		{
			TestTrue(TEXT("Failure stops remaining claims"),
				Progress.CompletedRoutes + Progress.FailedRoutes < Progress.TotalRoutes);
		}
		FString AbortError;
		TestTrue(TEXT("Failure staging abort"), Manager.AbortGeneration(AbortError));
		TestEqual(TEXT("Route failure preserves old Current"),
			Manager.GetCurrentInfo().ArtifactSetID, BaselineCurrentSetID);
	};

	ExpectRouteFailure(TEXT("position"), [](FRoutePreviewData& Data)
	{
		Data.PositionsENU[0].X = std::numeric_limits<float>::quiet_NaN();
	}, INDEX_NONE, 0);
	ExpectRouteFailure(TEXT("color"), [](FRoutePreviewData& Data)
	{
		Data.Colors[8] = FFloat16Color(FLinearColor(1.01f, 0.0f, 0.0f, 1.0f));
	}, INDEX_NONE, 8);
	ExpectRouteFailure(TEXT("writer"), [](FRoutePreviewData&) {}, 15, 15);

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> CancelData =
		MakeBatchData(*this, 128, 256, TEXT("cancel-source"));
	FRouteArtifactBuildIdentity CancelBuild;
	FRouteArtifactWorkspaceAuthorityGate CancelGate;
	FRouteArtifactGenerationHandle CancelHandle;
	FRouteWaypointBatchGenerator CancelBatch;
	TestTrue(TEXT("Begin cancel fixture"), BeginBatchFixture(
		*this, Manager, Root, CancelData, TEXT("cancel"), 4,
		CancelBuild, CancelGate, CancelHandle, CancelBatch, INDEX_NONE, 5));
	FPlatformProcess::SleepNoStats(0.02f);
	CancelBatch.Cancel();
	CancelBatch.Wait();
	const FRouteWaypointBatchProgress CancelProgress = CancelBatch.GetProgress();
	TestEqual(TEXT("Cancel terminal state"), CancelProgress.GenerationState,
		ERouteWaypointBatchState::Cancelled);
	TestTrue(TEXT("Cancel stops new claims"),
		CancelProgress.CompletedRoutes + CancelProgress.CancellationCount
			< CancelProgress.TotalRoutes);
	TestTrue(TEXT("Cancelled workers all exited"), CancelProgress.ActiveRoutes == 0);
	TestTrue(TEXT("Cancel staging cleanup"), Manager.AbortGeneration(Error));
	TestEqual(TEXT("Cancel preserves old Current"),
		Manager.GetCurrentInfo().ArtifactSetID, BaselineCurrentSetID);

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> FinalizeCancelData =
		MakeBatchData(*this, 3, 5, TEXT("finalize-cancel"));
	const FRouteWaypointGenerationSnapshot FinalizeCancelSnapshot =
		MakeSnapshot(*this, FinalizeCancelData);
	FRouteArtifactBuildIdentity FinalizeCancelBuild;
	FRouteArtifactWorkspaceAuthorityGate FinalizeCancelGate;
	FRouteArtifactGenerationHandle FinalizeCancelHandle;
	FRouteWaypointBatchGenerator FinalizeCancelBatch;
	TestTrue(TEXT("Begin Finalize cancellation fixture"), BeginBatchFixture(
		*this, Manager, Root, FinalizeCancelData, TEXT("finalize-cancel"), 2,
		FinalizeCancelBuild, FinalizeCancelGate, FinalizeCancelHandle,
		FinalizeCancelBatch));
	FinalizeCancelBatch.Wait();
	FRouteArtifactManifest FinalizeCancelDraft = MakeBatchDraft(
		FinalizeCancelBuild, FinalizeCancelSnapshot,
		FinalizeCancelHandle.ArtifactSetID, 5);
	int32 FinalizeCancelChecks = 0;
	TestFalse(TEXT("Finalize cancels at a route boundary"),
		Manager.FinalizeGeneratedRoutes(
			FinalizeCancelDraft, FinalizeCancelGate,
			FinalizeCancelSnapshot.MakeSourceValidationContext(), Error, nullptr,
			[&FinalizeCancelChecks]() { ++FinalizeCancelChecks; return true; }));
	TestEqual(TEXT("Finalize route-boundary cancellation check count"),
		FinalizeCancelChecks, 1);
	TestTrue(TEXT("Finalize cancellation has a bounded diagnostic"),
		Error.Contains(TEXT("Cancelled"), ESearchCase::IgnoreCase));
	TestEqual(TEXT("Finalize cancellation preserves old Current"),
		Manager.GetCurrentInfo().ArtifactSetID, BaselineCurrentSetID);

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> StaleData =
		MakeBatchData(*this, 3, 5, TEXT("stale-a"));
	const FRouteWaypointGenerationSnapshot StaleSnapshot = MakeSnapshot(*this, StaleData);
	FRouteArtifactBuildIdentity StaleBuild;
	FRouteArtifactWorkspaceAuthorityGate StaleGate;
	FRouteArtifactGenerationHandle StaleHandle;
	FRouteWaypointBatchGenerator StaleBatch;
	TestTrue(TEXT("Begin stale source fixture"), BeginBatchFixture(
		*this, Manager, Root, StaleData, TEXT("stale"), 2,
		StaleBuild, StaleGate, StaleHandle, StaleBatch, INDEX_NONE, 10));
	const FVector3f* SnapshotStorage = StaleSnapshot.RouteData->PositionsENU.GetData();
	// Simulate the active Route Authority replacing A with B while A workers run.
	StaleData = MakeBatchData(*this, 3, 5, TEXT("stale-b"));
	TestTrue(TEXT("Snapshot A remains alive after active pointer switches to B"),
		StaleSnapshot.RouteData->PositionsENU.GetData() == SnapshotStorage);
	StaleBatch.Wait();
	FRouteArtifactManifest StaleDraft = MakeBatchDraft(
		StaleBuild, StaleSnapshot, StaleHandle.ArtifactSetID, 5);
	StaleGate.ActiveSourceIdentity = StaleData->SourceIdentity;
	TestFalse(TEXT("Source change re-gate rejects Finalize"),
		Manager.FinalizeGeneratedRoutes(
			StaleDraft, StaleGate,
			StaleSnapshot.MakeSourceValidationContext(), Error));
	TestEqual(TEXT("Source change preserves old Current"),
		Manager.GetCurrentInfo().ArtifactSetID, BaselineCurrentSetID);

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> AssignmentData =
		MakeBatchData(*this, 3, 5, TEXT("assignment-stale"));
	const FRouteWaypointGenerationSnapshot AssignmentSnapshot =
		MakeSnapshot(*this, AssignmentData);
	FRouteArtifactBuildIdentity AssignmentBuild;
	FRouteArtifactWorkspaceAuthorityGate AssignmentGate;
	FRouteArtifactGenerationHandle AssignmentHandle;
	FRouteWaypointBatchGenerator AssignmentBatch;
	TestTrue(TEXT("Begin stale assignment fixture"), BeginBatchFixture(
		*this, Manager, Root, AssignmentData, TEXT("assignment-stale"), 2,
		AssignmentBuild, AssignmentGate, AssignmentHandle, AssignmentBatch));
	AssignmentBatch.Wait();
	FRouteArtifactManifest AssignmentDraft = MakeBatchDraft(
		AssignmentBuild, AssignmentSnapshot, AssignmentHandle.ArtifactSetID, 5);
	++AssignmentGate.FrozenAssignmentRevision;
	TestFalse(TEXT("Assignment re-gate rejects Finalize"),
		Manager.FinalizeGeneratedRoutes(
			AssignmentDraft, AssignmentGate,
			AssignmentSnapshot.MakeSourceValidationContext(), Error));
	TestEqual(TEXT("Assignment change preserves old Current"),
		Manager.GetCurrentInfo().ArtifactSetID, BaselineCurrentSetID);

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> ConsoleData =
		MakeBatchData(*this, 3, 5, TEXT("console-stale"));
	const FRouteWaypointGenerationSnapshot ConsoleSnapshot = MakeSnapshot(*this, ConsoleData);
	FRouteArtifactBuildIdentity ConsoleBuild;
	FRouteArtifactWorkspaceAuthorityGate ConsoleGate;
	FRouteArtifactGenerationHandle ConsoleHandle;
	FRouteWaypointBatchGenerator ConsoleBatch;
	TestTrue(TEXT("Begin stale ConsoleAccepted fixture"), BeginBatchFixture(
		*this, Manager, Root, ConsoleData, TEXT("console-stale"), 2,
		ConsoleBuild, ConsoleGate, ConsoleHandle, ConsoleBatch));
	ConsoleBatch.Wait();
	FRouteArtifactManifest ConsoleDraft = MakeBatchDraft(
		ConsoleBuild, ConsoleSnapshot, ConsoleHandle.ArtifactSetID, 5);
	ConsoleGate.bConsoleAccepted = false;
	TestFalse(TEXT("ConsoleAccepted re-gate rejects Finalize"),
		Manager.FinalizeGeneratedRoutes(
			ConsoleDraft, ConsoleGate,
			ConsoleSnapshot.MakeSourceValidationContext(), Error));
	TestEqual(TEXT("ConsoleAccepted change preserves old Current"),
		Manager.GetCurrentInfo().ArtifactSetID, BaselineCurrentSetID);

	Manager.Shutdown();
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointBatchScaleAndSnapshotTests,
	"DroneGCS.RouteWaypointBatch.2DC2A.SnapshotDiskAnd10K",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointBatchScaleAndSnapshotTests::RunTest(const FString& Parameters)
{
	const TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
		MakeBatchData(*this, 4, 32);
	const FVector3f* PositionStorage = Data->PositionsENU.GetData();
	const FFloat16Color* ColorStorage = Data->Colors.GetData();
	const FRouteWaypointGenerationSnapshot Snapshot = MakeSnapshot(*this, Data);
	TestTrue(TEXT("Snapshot shares PositionsENU allocation"),
		Snapshot.RouteData->PositionsENU.GetData() == PositionStorage);
	TestTrue(TEXT("Snapshot shares Colors allocation"),
		Snapshot.RouteData->Colors.GetData() == ColorStorage);

	int64 UpperBound = 0;
	int64 RequiredWithMargin = 0;
	FString Error;
	TArray<uint8> MaximumWidthRow;
	FRouteWaypointGeneratorError RowError;
	TestTrue(TEXT("Maximum-width v1 row formats"),
		RouteWaypointGenerator::FormatWaypointRow(
			MAX_int32, 255, 255, 255,
			-899999999, -1799999999, MIN_int64,
			MaximumWidthRow, RowError));
	TestEqual(TEXT("Derived maximum row byte count is exact"),
		static_cast<int64>(MaximumWidthRow.Num()),
		FRouteWaypointBatchGenerator::GetMaxWaypointRowBytesV1());
	TestEqual(TEXT("Strict v1 maximum row byte count"),
		FRouteWaypointBatchGenerator::GetMaxWaypointRowBytesV1(),
		static_cast<int64>(80));
	int64 MaxRouteFileBytes = 0;
	TestTrue(TEXT("Strict max route file formula"),
		FRouteWaypointBatchGenerator::ComputeMaxRouteFileBytesV1(
			9100, MaxRouteFileBytes, Error));
	TestEqual(TEXT("Strict 9100-frame route file upper bound"),
		MaxRouteFileBytes, static_cast<int64>(746211));
	TestTrue(TEXT("Overflow-safe output upper bound"),
		FRouteWaypointBatchGenerator::ComputeOutputUpperBound(9100, 1000, UpperBound, Error));
	TestFalse(TEXT("Insufficient disk rejected before Begin"),
		FRouteWaypointBatchGenerator::CheckDiskCapacity(
			9100, 1000, UpperBound, RequiredWithMargin, Error));
	TestTrue(TEXT("Disk preflight emits safe reason"),
		Error.Contains(TEXT("InsufficientDiskSpace")));

	TestEqual(TEXT("Default workers use profiled cap and physical cores"),
		FRouteWaypointBatchGenerator::GetDefaultWorkerCount(10000),
		FMath::Min3(10000, FPlatformMisc::NumberOfCores(),
			FRouteWaypointBatchGenerator::GetProfiledDefaultWorkerCap()));
	TestTrue(TEXT("Override safety bound is runtime-derived"),
		FRouteWaypointBatchGenerator::GetMaximumWorkerOverride(10000)
			<= FMath::Max(1, FPlatformMisc::NumberOfCores() * 2));

	const FString Root = BatchTempRoot(TEXT("10k"));
	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> ScaleData =
		MakeBatchData(*this, 2, 10000, TEXT("scale-10k"));
	const FRouteWaypointGenerationSnapshot ScaleSnapshot =
		MakeSnapshot(*this, ScaleData);
	FRouteArtifactWorkspaceManager Manager;
	FRouteArtifactBuildIdentity Build;
	FRouteArtifactWorkspaceAuthorityGate Gate;
	FRouteArtifactGenerationHandle Handle;
	FRouteWaypointBatchGenerator Batch;
	const int32 Workers = FMath::Min(4,
		FRouteWaypointBatchGenerator::GetMaximumWorkerOverride(10000));
	TestTrue(TEXT("Begin 10K small-frame batch"), BeginBatchFixture(
		*this, Manager, Root, ScaleData, TEXT("10k"), Workers,
		Build, Gate, Handle, Batch));
	Batch.Wait();
	const FRouteWaypointBatchProgress Progress = Batch.GetProgress();
	TestEqual(TEXT("10K route domain completed"), Progress.CompletedRoutes, 10000);
	TestEqual(TEXT("10K worker task count remains bounded"), Progress.WorkerCount, Workers);
	TestTrue(TEXT("10K peak active workers bounded"), Progress.PeakActiveWorkers <= Workers);
	TestTrue(TEXT("10K peak open route files bounded"), Progress.PeakOpenRouteFiles <= Workers);
	TestTrue(TEXT("10K no route failures"), Progress.RouteFailures == 0);
	AddInfo(FString::Printf(
		TEXT("C2A_METRICS PhysicalCores=%d DefaultWorkers10K=%d TestWorkers=%d PeakActiveWorkers=%d PeakOpenRouteFiles=%d Routes=%d Rows=%lld Bytes=%lld"),
		FPlatformMisc::NumberOfCores(),
		FRouteWaypointBatchGenerator::GetDefaultWorkerCount(10000),
		Workers,
		Progress.PeakActiveWorkers,
		Progress.PeakOpenRouteFiles,
		Progress.CompletedRoutes,
		Progress.RowsWritten,
		Progress.BytesWritten));
	FRouteArtifactManifest ScaleDraft = MakeBatchDraft(
		Build, ScaleSnapshot, Handle.ArtifactSetID, 10000);
	TestTrue(TEXT("10K SourceBound finalize"),
		Manager.FinalizeGeneratedRoutes(
			ScaleDraft, Gate,
			ScaleSnapshot.MakeSourceValidationContext(), Error));
	TestEqual(TEXT("10K validation reads each physical route exactly once"),
		Manager.GetTestFinalizeRouteValidationReadCount(), static_cast<int64>(10000));
	Manager.Shutdown();
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif
