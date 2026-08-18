#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceManager.h"
#include "RouteArtifactWorkspacePaths.h"

namespace
{
	FRouteArtifactDigest256 WorkspaceHash(FAutomationTestBase& Test, const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FRouteArtifactDigest256 Digest;
		FString Error;
		Test.TestTrue(TEXT("Synthetic digest"), FRouteArtifactSha256::HashBytes(
			Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest, Error));
		return Digest;
	}

	FString MakeTempRoot(const FString& Label)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("RouteArtifactWorkspaceTests"),
			Label + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	FRouteArtifactBuildIdentity MakeBuild(FAutomationTestBase& Test, const FString& Label)
	{
		FRouteArtifactBuildIdentity Build;
		Build.AssignmentBinding.FleetSessionID = TEXT("fleet-session");
		Build.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("authority-session");
		Build.AssignmentBinding.AssignmentRevision = 7;
		Build.AssignmentBinding.AssignmentDigest = WorkspaceHash(Test, TEXT("assignment"));
		Build.SourceDatasetDigest = WorkspaceHash(Test, TEXT("source"));
		Build.WaypointGeneratorVersion = TEXT("synthetic-workspace-test-v1");
		Build.GeneratorInputDigest = WorkspaceHash(Test, Label);
		return Build;
	}

	FRouteArtifactSourceIdentity MakeSource(
		FAutomationTestBase& Test,
		const FRouteArtifactDigest256& Digest,
		const FString& Name = TEXT("show.npz"))
	{
		FRouteArtifactSourceIdentity Source;
		FString Error;
		Test.TestTrue(TEXT("Synthetic source identity"),
			FRouteArtifactSourceIdentity::Create(Digest, 1234, Name, Source, Error));
		return Source;
	}

	FRouteArtifactWorkspaceAuthorityGate MakeGate(
		FAutomationTestBase& Test,
		const FRouteArtifactBuildIdentity& Build,
		const int32 RouteCount)
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
		Gate.ActiveSourceIdentity = MakeSource(Test, Build.SourceDatasetDigest);
		Gate.RouteSlotCount = RouteCount;
		return Gate;
	}

	bool WriteBytes(const FString& Path, const TArray<uint8>& Bytes)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	TArray<uint8> TextBytes(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	bool BuildSyntheticManifest(
		FAutomationTestBase& Test,
		FRouteArtifactWorkspaceManager& Manager,
		const FRouteArtifactBuildIdentity& Build,
		const FRouteArtifactWorkspaceAuthorityGate& Gate,
		const FRouteArtifactGenerationHandle& Handle,
		const FString& Label,
		FRouteArtifactManifest& OutManifest)
	{
		OutManifest = FRouteArtifactManifest{};
		OutManifest.ArtifactSetID = Handle.ArtifactSetID;
		OutManifest.CreatedAtUtc = 1700000000000;
		OutManifest.AssignmentBinding = Build.AssignmentBinding;
		OutManifest.FleetRevisionAtCapture = 19;
		OutManifest.RouteDatasetGeneration = 3;
		OutManifest.CompletionMode = ERouteAssignmentCompletionMode::Full;
		OutManifest.FreezePolicy = ERouteAssignmentFreezePolicy::RequireFull;
		OutManifest.MatchedCount = Gate.RouteSlotCount;
		OutManifest.SourceIdentity = Gate.ActiveSourceIdentity;
		OutManifest.FrameCount = 2;
		OutManifest.RouteSlotCount = Gate.RouteSlotCount;
		OutManifest.WaypointGeneratorVersion = Build.WaypointGeneratorVersion;
		OutManifest.GeneratorInputDigest = Build.GeneratorInputDigest;

		FString Error;
		for (int32 Slot = 0; Slot < Gate.RouteSlotCount; ++Slot)
		{
			FRouteArtifactMatchedMapping& Mapping =
				OutManifest.MatchedMapping.AddDefaulted_GetRef();
			Mapping.DroneID = FString::Printf(TEXT("drone-%06d"), Slot);
			Mapping.RouteSlotIndex = Slot;

			FString RoutePath;
			if (!Manager.ResolveSafeStagingRoutePath(Slot, RoutePath, Error))
			{
				Test.AddError(Error);
				return false;
			}
			const TArray<uint8> Bytes = TextBytes(
				TEXT("QGC WPL 110\r\n")
				TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1\r\n")
				TEXT("1\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1"));
			if (!WriteBytes(RoutePath, Bytes))
			{
				Test.AddError(TEXT("Could not write synthetic route fixture."));
				return false;
			}

			FRouteArtifactRecord& Record = OutManifest.Routes.AddDefaulted_GetRef();
			Record.RouteSlotIndex = Slot;
			if (!RouteArtifactWorkspacePaths::BuildRouteRelativePath(
				Slot, Record.RelativePath, Error)
				|| !FRouteArtifactSha256::HashFileStreaming(
					RoutePath, Record.RouteArtifactDigest, Record.FileByteSize, Error))
			{
				Test.AddError(Error);
				return false;
			}
		}
		if (!RouteArtifactManifest::ComputeArtifactSetDigest(
			OutManifest, OutManifest.ArtifactSetDigest, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return true;
	}

	bool BeginFixture(
		FAutomationTestBase& Test,
		FRouteArtifactWorkspaceManager& Manager,
		const FString& Root,
		const FString& Label,
		const int32 RouteCount,
		FRouteArtifactBuildIdentity& OutBuild,
		FRouteArtifactWorkspaceAuthorityGate& OutGate,
		FRouteArtifactGenerationHandle& OutHandle,
		FRouteArtifactManifest& OutManifest)
	{
		FString Error;
		if (Manager.GetConfiguredWorkspaceRoot().IsEmpty()
			&& !Manager.SetConfiguredWorkspaceRoot(Root, Error))
		{
			Test.AddError(Error);
			return false;
		}
		OutBuild = MakeBuild(Test, Label);
		OutGate = MakeGate(Test, OutBuild, RouteCount);
		if (!Manager.BeginGeneration(OutBuild, OutGate, OutHandle, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return BuildSyntheticManifest(
			Test, Manager, OutBuild, OutGate, OutHandle, Label, OutManifest);
	}

	bool FinalizeFixture(
		FAutomationTestBase& Test,
		FRouteArtifactWorkspaceManager& Manager,
		const FRouteArtifactManifest& Manifest,
		const FRouteArtifactWorkspaceAuthorityGate& Gate)
	{
		FString Error;
		const bool bResult = Manager.FinalizeGeneration(Manifest, Gate, Error);
		if (!bResult) Test.AddError(Error);
		return bResult;
	}

	using FDirectoryImage = TMap<FString, TArray<uint8>>;

	FDirectoryImage CaptureDirectory(const FString& Root)
	{
		FDirectoryImage Image;
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*"), true, false);
		for (const FString& File : Files)
		{
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *File))
			{
				FString Relative = File;
				FPaths::MakePathRelativeTo(Relative, *(Root + TEXT("/")));
				FPaths::NormalizeFilename(Relative);
				Image.Add(Relative, MoveTemp(Bytes));
			}
		}
		return Image;
	}

	bool RestoreDirectory(const FString& Root, const FDirectoryImage& Image)
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		for (const TPair<FString, TArray<uint8>>& Entry : Image)
		{
			if (!WriteBytes(FPaths::Combine(Root, Entry.Key), Entry.Value)) return false;
		}
		return true;
	}

	bool ImagesEqual(const FDirectoryImage& Left, const FDirectoryImage& Right)
	{
		if (Left.Num() != Right.Num()) return false;
		for (const TPair<FString, TArray<uint8>>& Entry : Left)
		{
			const TArray<uint8>* Other = Right.Find(Entry.Key);
			if (!Other || *Other != Entry.Value) return false;
		}
		return true;
	}

	void DeleteTempRoot(const FString& Root)
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactWorkspaceLifecycleTests,
	"DroneGCS.RouteArtifact.2C3A.WorkspaceLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactWorkspaceLifecycleTests::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot(TEXT("Lifecycle"));
	FString Error;
	FRouteArtifactWorkspaceManager Manager;
	TestTrue(TEXT("Configured user root accepted"),
		Manager.SetConfiguredWorkspaceRoot(Root, Error));
	FString Resolved;
	TestTrue(TEXT("Configured root resolves"), Manager.ResolveWorkspaceRoot(Resolved, Error));
	TestEqual(TEXT("Resolved configured root"), Resolved, Root.Replace(TEXT("\\"), TEXT("/")));

	FRouteArtifactBuildIdentity BuildA;
	FRouteArtifactWorkspaceAuthorityGate GateA;
	FRouteArtifactGenerationHandle HandleA;
	FRouteArtifactManifest ManifestA;
	if (!BeginFixture(*this, Manager, Root, TEXT("A"), 5,
		BuildA, GateA, HandleA, ManifestA)) return false;
	TestTrue(TEXT("Current untouched and absent during first Begin"),
		!IFileManager::Get().DirectoryExists(*FPaths::Combine(Root, TEXT("Current"))));
	TestTrue(TEXT("Current/Staging fixed layout"),
		HandleA.StagingRoot.StartsWith(FPaths::Combine(Root, TEXT("Staging"))));
	TestEqual(TEXT("Attempt ID is GUID digits"), HandleA.AttemptID.Len(), 32);
	TestTrue(TEXT("READY absent before successful Finalize"),
		!IFileManager::Get().FileExists(*FPaths::Combine(HandleA.StagingRoot, TEXT("READY"))));

	FRouteArtifactWorkspaceManager Contender;
	TestFalse(TEXT("Contender root recovery sees active writer"),
		Contender.SetConfiguredWorkspaceRoot(Root, Error));
	FRouteArtifactGenerationHandle RejectedHandle;
	TestTrue(TEXT("First Finalize"), FinalizeFixture(*this, Manager, ManifestA, GateA));
	TestEqual(TEXT("Current Ready after first Finalize"),
		Manager.GetCurrentStatus(), ERouteArtifactCurrentStatus::Ready);
	TestEqual(TEXT("Generation None after Finalize"),
		Manager.GetGenerationStatus(), ERouteArtifactGenerationStatus::None);
	TestFalse(TEXT("Swap flag cleared"), Manager.IsCurrentSwapInProgress());
	const FString Current = FPaths::Combine(Root, TEXT("Current"));
	TestTrue(TEXT("READY exists after successful Finalize"),
		IFileManager::Get().FileExists(*FPaths::Combine(Current, TEXT("READY"))));
	FRouteArtifactManifest Loaded;
	TestTrue(TEXT("First Current validates"),
		Manager.LoadAndValidateCurrentArtifact(Loaded, Error));
	TestEqual(TEXT("READY/Manifest set identity"), Loaded.ArtifactSetID, HandleA.ArtifactSetID);

	const FDirectoryImage CurrentA = CaptureDirectory(Current);
	FRouteArtifactBuildIdentity BuildB;
	FRouteArtifactWorkspaceAuthorityGate GateB;
	FRouteArtifactGenerationHandle HandleB;
	FRouteArtifactManifest ManifestB;
	TestTrue(TEXT("Begin B after Finalize"), BeginFixture(*this, Manager, Root, TEXT("B"), 3,
		BuildB, GateB, HandleB, ManifestB));
	TestTrue(TEXT("Begin B preserves old Current byte-for-byte"),
		ImagesEqual(CurrentA, CaptureDirectory(Current)));
	TestFalse(TEXT("Root change rejected during active generation"),
		Manager.SetConfiguredWorkspaceRoot(MakeTempRoot(TEXT("Other")), Error));
	TestTrue(TEXT("Replacement Finalize"), FinalizeFixture(*this, Manager, ManifestB, GateB));
	TestTrue(TEXT("Replacement Current validates"),
		Manager.LoadAndValidateCurrentArtifact(Loaded, Error));
	TestEqual(TEXT("Replacement route count"), Loaded.RouteSlotCount, 3);
	FString OldHighPath;
	RouteArtifactWorkspacePaths::BuildRouteRelativePath(4, OldHighPath, Error);
	TestFalse(TEXT("Old high-number route does not remain"),
		IFileManager::Get().FileExists(*FPaths::Combine(Current, OldHighPath)));
	TestFalse(TEXT("Old manifest identity replaced"), Loaded.ArtifactSetID == ManifestA.ArtifactSetID);

	// Abort cleans only the new attempt, preserves Current, and releases ownership.
	const FDirectoryImage CurrentB = CaptureDirectory(Current);
	FRouteArtifactBuildIdentity BuildAbort;
	FRouteArtifactWorkspaceAuthorityGate GateAbort;
	FRouteArtifactGenerationHandle HandleAbort;
	FRouteArtifactManifest ManifestAbort;
	TestTrue(TEXT("Begin abort fixture"), BeginFixture(*this, Manager, Root, TEXT("abort"), 2,
		BuildAbort, GateAbort, HandleAbort, ManifestAbort));
	TestTrue(TEXT("Abort succeeds"), Manager.AbortGeneration(Error));
	TestFalse(TEXT("Attempt removed by Abort"),
		IFileManager::Get().DirectoryExists(*HandleAbort.StagingRoot));
	TestTrue(TEXT("Abort preserves Current"), ImagesEqual(CurrentB, CaptureDirectory(Current)));
	TestFalse(TEXT("Repeated Abort is explicit safe failure"), Manager.AbortGeneration(Error));
	TestTrue(TEXT("Contender root accepted after writer release"),
		Contender.SetConfiguredWorkspaceRoot(Root, Error));
	TestTrue(TEXT("Writer released after Abort"),
		Contender.BeginGeneration(BuildAbort, GateAbort, RejectedHandle, Error));
	TestTrue(TEXT("Contender Abort"), Contender.AbortGeneration(Error));

	// Failure injection: Current->previous succeeds, attempt->Current fails, rollback restores A/B.
	FRouteArtifactBuildIdentity BuildRollback;
	FRouteArtifactWorkspaceAuthorityGate GateRollback;
	FRouteArtifactGenerationHandle HandleRollback;
	FRouteArtifactManifest ManifestRollback;
	TestTrue(TEXT("Begin rollback fixture"), BeginFixture(*this, Manager, Root, TEXT("rollback"), 2,
		BuildRollback, GateRollback, HandleRollback, ManifestRollback));
	Manager.SetTestFailAttemptPromotion(true);
	TestFalse(TEXT("Injected promotion move fails"),
		Manager.FinalizeGeneration(ManifestRollback, GateRollback, Error));
	Manager.SetTestFailAttemptPromotion(false);
	TestTrue(TEXT("Promotion rollback restores old Current byte-for-byte"),
		ImagesEqual(CurrentB, CaptureDirectory(Current)));
	TestFalse(TEXT("Failed promotion never leaves READY staging attempt"),
		IFileManager::Get().DirectoryExists(*HandleRollback.StagingRoot));

	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactWorkspaceFailureAndGateTests,
	"DroneGCS.RouteArtifact.2C3A.FailureAndFinalizeGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactWorkspaceFailureAndGateTests::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot(TEXT("Failures"));
	FRouteArtifactWorkspaceManager Manager;
	FString Error;
	TestTrue(TEXT("Failure root"), Manager.SetConfiguredWorkspaceRoot(Root, Error));

	FRouteArtifactBuildIdentity BaseBuild;
	FRouteArtifactWorkspaceAuthorityGate BaseGate;
	FRouteArtifactGenerationHandle BaseHandle;
	FRouteArtifactManifest BaseManifest;
	if (!BeginFixture(*this, Manager, Root, TEXT("base"), 2,
		BaseBuild, BaseGate, BaseHandle, BaseManifest)
		|| !FinalizeFixture(*this, Manager, BaseManifest, BaseGate)) return false;
	const FString Current = FPaths::Combine(Root, TEXT("Current"));
	const FDirectoryImage Baseline = CaptureDirectory(Current);

	auto ExpectFailedPreserves = [this, &Manager, &Root, &Current, &Baseline](
		const FString& Label,
		TFunction<void(FRouteArtifactManifest&, FRouteArtifactWorkspaceAuthorityGate&,
			const FRouteArtifactGenerationHandle&)> Mutate)
	{
		FRouteArtifactBuildIdentity Build;
		FRouteArtifactWorkspaceAuthorityGate Gate;
		FRouteArtifactGenerationHandle Handle;
		FRouteArtifactManifest Manifest;
		if (!BeginFixture(*this, Manager, Root, Label, 2, Build, Gate, Handle, Manifest))
		{
			return false;
		}
		Mutate(Manifest, Gate, Handle);
		FString LocalError;
		const bool bRejected = !Manager.FinalizeGeneration(Manifest, Gate, LocalError);
		TestTrue(*FString::Printf(TEXT("%s rejected"), *Label), bRejected);
		TestTrue(*FString::Printf(TEXT("%s preserves Current"), *Label),
			ImagesEqual(Baseline, CaptureDirectory(Current)));
		TestFalse(*FString::Printf(TEXT("%s attempt cleaned"), *Label),
			IFileManager::Get().DirectoryExists(*Handle.StagingRoot));
		return bRejected;
	};

	ExpectFailedPreserves(TEXT("missing-route"), [](FRouteArtifactManifest& Manifest,
		FRouteArtifactWorkspaceAuthorityGate&, const FRouteArtifactGenerationHandle& Handle)
	{
		IFileManager::Get().Delete(*FPaths::Combine(Handle.StagingRoot,
			Manifest.Routes.Last().RelativePath), false, true, true);
	});
	ExpectFailedPreserves(TEXT("hash-mismatch"), [](FRouteArtifactManifest& Manifest,
		FRouteArtifactWorkspaceAuthorityGate&, const FRouteArtifactGenerationHandle& Handle)
	{
		WriteBytes(FPaths::Combine(Handle.StagingRoot, Manifest.Routes[0].RelativePath),
			TextBytes(TEXT("changed")));
	});
	ExpectFailedPreserves(TEXT("invalid-manifest"), [](FRouteArtifactManifest& Manifest,
		FRouteArtifactWorkspaceAuthorityGate&, const FRouteArtifactGenerationHandle&)
	{
		Manifest.FrameCount = 0;
	});
	ExpectFailedPreserves(TEXT("wrong-set-id"), [](FRouteArtifactManifest& Manifest,
		FRouteArtifactWorkspaceAuthorityGate&, const FRouteArtifactGenerationHandle&)
	{
		Manifest.ArtifactSetID[0] = Manifest.ArtifactSetID[0] == TEXT('a') ? TEXT('b') : TEXT('a');
	});
	ExpectFailedPreserves(TEXT("wrong-binding"), [](FRouteArtifactManifest& Manifest,
		FRouteArtifactWorkspaceAuthorityGate&, const FRouteArtifactGenerationHandle&)
	{
		Manifest.AssignmentBinding.FleetSessionID = TEXT("other-fleet");
	});

	auto GateChange = [&ExpectFailedPreserves](const FString& Label,
		TFunction<void(FRouteArtifactWorkspaceAuthorityGate&)> Change)
	{
		return ExpectFailedPreserves(Label,
			[Change](FRouteArtifactManifest&, FRouteArtifactWorkspaceAuthorityGate& Gate,
				const FRouteArtifactGenerationHandle&) { Change(Gate); });
	};
	GateChange(TEXT("gate-fleet-session"), [](FRouteArtifactWorkspaceAuthorityGate& Gate)
	{
		Gate.FrozenFleetSessionID = TEXT("other-fleet");
	});
	GateChange(TEXT("gate-authority-session"), [](FRouteArtifactWorkspaceAuthorityGate& Gate)
	{
		Gate.FrozenAssignmentAuthoritySessionID = TEXT("other-authority");
	});
	GateChange(TEXT("gate-assignment-revision"), [](FRouteArtifactWorkspaceAuthorityGate& Gate)
	{
		++Gate.FrozenAssignmentRevision;
	});
	GateChange(TEXT("gate-assignment-digest"), [this](FRouteArtifactWorkspaceAuthorityGate& Gate)
	{
		Gate.ConsoleAcceptedBinding.AssignmentDigest = WorkspaceHash(*this, TEXT("other-digest"));
	});
	GateChange(TEXT("gate-source-digest"), [this](FRouteArtifactWorkspaceAuthorityGate& Gate)
	{
		Gate.ActiveSourceIdentity = MakeSource(*this, WorkspaceHash(*this, TEXT("other-source")));
	});

	// FleetRevision is intentionally absent from the live gate. Audit-only growth cannot veto.
	FRouteArtifactBuildIdentity FleetRevisionBuild;
	FRouteArtifactWorkspaceAuthorityGate FleetRevisionGate;
	FRouteArtifactGenerationHandle FleetRevisionHandle;
	FRouteArtifactManifest FleetRevisionManifest;
	TestTrue(TEXT("Begin FleetRevision-only fixture"), BeginFixture(*this, Manager, Root,
		TEXT("fleet-revision-only"), 2, FleetRevisionBuild, FleetRevisionGate,
		FleetRevisionHandle, FleetRevisionManifest));
	FleetRevisionManifest.FleetRevisionAtCapture = 999;
	FRouteArtifactDigest256 UpdatedDigest;
	TestTrue(TEXT("Recompute audit digest"), RouteArtifactManifest::ComputeArtifactSetDigest(
		FleetRevisionManifest, UpdatedDigest, Error));
	FleetRevisionManifest.ArtifactSetDigest = UpdatedDigest;
	TestTrue(TEXT("FleetRevision audit growth alone does not block"),
		Manager.FinalizeGeneration(FleetRevisionManifest, FleetRevisionGate, Error));

	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactWorkspaceCurrentValidationTests,
	"DroneGCS.RouteArtifact.2C3A.ReadyAndCurrentValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactWorkspaceCurrentValidationTests::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot(TEXT("Validation"));
	FRouteArtifactWorkspaceManager Manager;
	FRouteArtifactBuildIdentity Build;
	FRouteArtifactWorkspaceAuthorityGate Gate;
	FRouteArtifactGenerationHandle Handle;
	FRouteArtifactManifest Manifest;
	if (!BeginFixture(*this, Manager, Root, TEXT("validation"), 2,
		Build, Gate, Handle, Manifest)
		|| !FinalizeFixture(*this, Manager, Manifest, Gate)) return false;
	const FString Current = FPaths::Combine(Root, TEXT("Current"));
	const FDirectoryImage ValidImage = CaptureDirectory(Current);
	FString Error;
	FRouteArtifactManifest Loaded;
	TestTrue(TEXT("Valid Current is Ready"),
		Manager.LoadAndValidateCurrentArtifact(Loaded, Error));

	TArray<uint8> ReadyBytes;
	TestTrue(TEXT("READY readable"), FFileHelper::LoadFileToArray(
		ReadyBytes, *FPaths::Combine(Current, TEXT("READY"))));
	FString ReadySetID;
	FRouteArtifactDigest256 ReadyDigest;
	TestTrue(TEXT("Strict READY parses"), FRouteArtifactWorkspaceManager::ParseReadyMarker(
		ReadyBytes, ReadySetID, ReadyDigest, Error));
	TestEqual(TEXT("READY set id matches"), ReadySetID, Manifest.ArtifactSetID);
	TestTrue(TEXT("READY digest matches"), ReadyDigest == Manifest.ArtifactSetDigest);
	TArray<uint8> MalformedReady = ReadyBytes;
	MalformedReady.Append(TextBytes(TEXT("Unknown=field\n")));
	TestFalse(TEXT("READY unknown field rejected"),
		FRouteArtifactWorkspaceManager::ParseReadyMarker(
			MalformedReady, ReadySetID, ReadyDigest, Error));

	auto ExpectInvalid = [this, &Manager, &Root, &Current, &ValidImage](
		const FString& Label, TFunction<void()> Corrupt)
	{
		TestTrue(*FString::Printf(TEXT("Restore before %s"), *Label),
			RestoreDirectory(Current, ValidImage));
		Corrupt();
		FRouteArtifactManifest LoadedManifest;
		FString LocalError;
		TestFalse(*FString::Printf(TEXT("%s Current rejected"), *Label),
			Manager.LoadAndValidateCurrentArtifact(LoadedManifest, LocalError));
		TestEqual(*FString::Printf(TEXT("%s status Invalid"), *Label),
			Manager.GetCurrentStatus(), ERouteArtifactCurrentStatus::Invalid);
		TestTrue(*FString::Printf(TEXT("%s is not auto-deleted"), *Label),
			IFileManager::Get().DirectoryExists(*Current));
	};
	ExpectInvalid(TEXT("missing READY"), [&Current]()
	{
		IFileManager::Get().Delete(*FPaths::Combine(Current, TEXT("READY")), false, true, true);
	});
	ExpectInvalid(TEXT("missing manifest"), [&Current]()
	{
		IFileManager::Get().Delete(*FPaths::Combine(Current, TEXT("manifest.json")), false, true, true);
	});
	ExpectInvalid(TEXT("READY digest mismatch"), [this, &Current, &Manifest]()
	{
		TArray<uint8> Bytes;
		FString LocalError;
		FRouteArtifactWorkspaceManager::SerializeReadyMarker(
			Manifest.ArtifactSetID, WorkspaceHash(*this, TEXT("other-ready")), Bytes, LocalError);
		WriteBytes(FPaths::Combine(Current, TEXT("READY")), Bytes);
	});
	ExpectInvalid(TEXT("malformed READY"), [&Current]()
	{
		WriteBytes(FPaths::Combine(Current, TEXT("READY")), TextBytes(TEXT("READY\n")));
	});
	ExpectInvalid(TEXT("manifest corruption"), [&Current]()
	{
		WriteBytes(FPaths::Combine(Current, TEXT("manifest.json")), TextBytes(TEXT("{}")));
	});
	ExpectInvalid(TEXT("route corruption"), [&Current, &Manifest]()
	{
		WriteBytes(FPaths::Combine(Current, Manifest.Routes[0].RelativePath),
			TextBytes(TEXT("corrupt")));
	});
	ExpectInvalid(TEXT("missing route"), [&Current, &Manifest]()
	{
		IFileManager::Get().Delete(*FPaths::Combine(Current,
			Manifest.Routes[0].RelativePath), false, true, true);
	});

	DeleteTempRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactWorkspaceTenThousandDomainTests,
	"DroneGCS.RouteArtifact.2C3A.TenThousandDomainLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactWorkspaceTenThousandDomainTests::RunTest(const FString& Parameters)
{
	const FString Root = MakeTempRoot(TEXT("TenThousand"));
	FRouteArtifactWorkspaceManager Manager;
	FString Error;
	TestTrue(TEXT("10K root"), Manager.SetConfiguredWorkspaceRoot(Root, Error));
	const FRouteArtifactBuildIdentity Build = MakeBuild(*this, TEXT("10k"));
	const FRouteArtifactWorkspaceAuthorityGate Gate = MakeGate(*this, Build, 10000);
	FRouteArtifactGenerationHandle Handle;
	TestTrue(TEXT("10K domain Begin"), Manager.BeginGeneration(Build, Gate, Handle, Error));
	FString FirstPath;
	FString LastPath;
	TestTrue(TEXT("10K first route resolves"),
		Manager.ResolveSafeStagingRoutePath(0, FirstPath, Error));
	TestTrue(TEXT("10K last route resolves"),
		Manager.ResolveSafeStagingRoutePath(9999, LastPath, Error));
	TestFalse(TEXT("10K out-of-domain route rejects"),
		Manager.ResolveSafeStagingRoutePath(10000, LastPath, Error));
	TestTrue(TEXT("10K lifecycle Abort"), Manager.AbortGeneration(Error));
	TestFalse(TEXT("10K attempt removed"), IFileManager::Get().DirectoryExists(*Handle.StagingRoot));
	DeleteTempRoot(Root);
	return true;
}

#endif
