#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceManager.h"
#include "RouteArtifactWorkspacePaths.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
	FString RecoveryTempRoot(const FString& Label)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("RouteArtifactRecoveryTests"),
			Label + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	FString RecoveryAttemptID()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	}

	FRouteArtifactDigest256 RecoveryHash(FAutomationTestBase& Test, const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FRouteArtifactDigest256 Digest;
		FString Error;
		Test.TestTrue(TEXT("Recovery fixture hash"), FRouteArtifactSha256::HashBytes(
			Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest, Error));
		return Digest;
	}

	FRouteArtifactBuildIdentity RecoveryBuild(FAutomationTestBase& Test, const FString& Label)
	{
		FRouteArtifactBuildIdentity Build;
		Build.AssignmentBinding.FleetSessionID = TEXT("recovery-fleet");
		Build.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("recovery-authority");
		Build.AssignmentBinding.AssignmentRevision = 4;
		Build.AssignmentBinding.AssignmentDigest = RecoveryHash(Test, TEXT("assignment"));
		Build.SourceDatasetDigest = RecoveryHash(Test, TEXT("source"));
		Build.WaypointGeneratorVersion = TEXT("recovery-synthetic-v1");
		Build.GeneratorInputDigest = RecoveryHash(Test, Label);
		return Build;
	}

	FRouteArtifactWorkspaceAuthorityGate RecoveryGate(
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
		FString Error;
		Test.TestTrue(TEXT("Recovery source identity"),
			FRouteArtifactSourceIdentity::Create(
				Build.SourceDatasetDigest, 321, TEXT("recovery.npz"),
				Gate.ActiveSourceIdentity, Error));
		Gate.RouteSlotCount = RouteCount;
		return Gate;
	}

	TArray<uint8> RecoveryBytes(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	bool RecoveryWrite(const FString& Path, const TArray<uint8>& Bytes)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	bool CreateReadyArtifact(
		FAutomationTestBase& Test,
		const FString& Root,
		const FString& Label,
		const int32 RouteCount,
		FRouteArtifactManifest& OutManifest)
	{
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		if (!Manager.SetConfiguredWorkspaceRoot(Root, Error))
		{
			Test.AddError(Error);
			return false;
		}
		const FRouteArtifactBuildIdentity Build = RecoveryBuild(Test, Label);
		const FRouteArtifactWorkspaceAuthorityGate Gate = RecoveryGate(Test, Build, RouteCount);
		FRouteArtifactGenerationHandle Handle;
		if (!Manager.BeginGeneration(Build, Gate, Handle, Error))
		{
			Test.AddError(Error);
			return false;
		}

		OutManifest = FRouteArtifactManifest{};
		OutManifest.ArtifactSetID = Handle.ArtifactSetID;
		OutManifest.CreatedAtUtc = 1700000001000;
		OutManifest.AssignmentBinding = Build.AssignmentBinding;
		OutManifest.FleetRevisionAtCapture = 8;
		OutManifest.RouteDatasetGeneration = 2;
		OutManifest.CompletionMode = ERouteAssignmentCompletionMode::Full;
		OutManifest.FreezePolicy = ERouteAssignmentFreezePolicy::RequireFull;
		OutManifest.MatchedCount = RouteCount;
		OutManifest.SourceIdentity = Gate.ActiveSourceIdentity;
		OutManifest.FrameCount = 1;
		OutManifest.RouteSlotCount = RouteCount;
		OutManifest.WaypointGeneratorVersion = Build.WaypointGeneratorVersion;
		OutManifest.GeneratorInputDigest = Build.GeneratorInputDigest;

		for (int32 Slot = 0; Slot < RouteCount; ++Slot)
		{
			FString RoutePath;
			if (!Manager.ResolveSafeStagingRoutePath(Slot, RoutePath, Error)
				|| !RecoveryWrite(RoutePath, RecoveryBytes(
					TEXT("QGC WPL 110\r\n")
					TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1"))))
			{
				Test.AddError(Error.IsEmpty() ? TEXT("Route fixture write failed.") : Error);
				return false;
			}
			FRouteArtifactMatchedMapping& Mapping =
				OutManifest.MatchedMapping.AddDefaulted_GetRef();
			Mapping.DroneID = FString::Printf(TEXT("recovery-drone-%06d"), Slot);
			Mapping.RouteSlotIndex = Slot;
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
			OutManifest, OutManifest.ArtifactSetDigest, Error)
			|| !Manager.FinalizeGeneration(OutManifest, Gate, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return true;
	}

	bool CopyTree(const FString& Destination, const FString& Source)
	{
		return FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(
			*Destination, *Source, true);
	}

	bool MoveTree(const FString& Destination, const FString& Source)
	{
		return FPlatformFileManager::Get().GetPlatformFile().MoveFile(
			*Destination, *Source);
	}

	void DeleteRecoveryRoot(const FString& Root)
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}

	bool LoadReady(
		FRouteArtifactWorkspaceManager& Manager,
		FRouteArtifactManifest& OutManifest)
	{
		FString Error;
		return Manager.LoadAndValidateCurrentArtifact(OutManifest, Error);
	}

	using FRecoveryTreeImage = TMap<FString, TArray<uint8>>;

	FRecoveryTreeImage CaptureRecoveryTree(const FString& Root)
	{
		FRecoveryTreeImage Image;
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

	bool SameRecoveryTree(
		const FRecoveryTreeImage& Left,
		const FRecoveryTreeImage& Right)
	{
		if (Left.Num() != Right.Num()) return false;
		for (const TPair<FString, TArray<uint8>>& Entry : Left)
		{
			const TArray<uint8>* Other = Right.Find(Entry.Key);
			if (!Other || *Other != Entry.Value) return false;
		}
		return true;
	}

#if PLATFORM_WINDOWS
	bool CreateFilesystemLink(
		const FString& Link,
		const FString& Target,
		const bool bDirectory)
	{
		DWORD Flags = bDirectory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
		Flags |= 0x2; // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
		return !!::CreateSymbolicLinkW(*Link, *Target, Flags);
	}

	void RemoveFilesystemLink(const FString& Link, const bool bDirectory)
	{
		if (bDirectory) ::RemoveDirectoryW(*Link);
		else ::DeleteFileW(*Link);
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactCrashRecoveryTests,
	"DroneGCS.RouteArtifact.2C3B.CrashRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactCrashRecoveryTests::RunTest(const FString& Parameters)
{
	FString Error;
	auto Recover = [this](const FString& Root,
		ERouteArtifactRecoveryStatus ExpectedStatus,
		FRouteArtifactWorkspaceManager& OutManager)
	{
		FString LocalError;
		const bool bSet = OutManager.SetConfiguredWorkspaceRoot(Root, LocalError);
		TestTrue(TEXT("Recovery root accepted"), bSet);
		TestEqual(TEXT("Recovery status"),
			OutManager.GetLastRecoveryResult().Status, ExpectedStatus);
		return bSet;
	};

	// Crash A: valid Current wins; incomplete orphan is cleaned.
	{
		const FString Root = RecoveryTempRoot(TEXT("CrashA"));
		FRouteArtifactManifest A;
		if (!CreateReadyArtifact(*this, Root, TEXT("A"), 2, A)) return false;
		const FString Current = FPaths::Combine(Root, TEXT("Current"));
		const FRecoveryTreeImage BeforeRecovery = CaptureRecoveryTree(Current);
		const FString Orphan = FPaths::Combine(Root, TEXT("Staging"), RecoveryAttemptID());
		TestTrue(TEXT("Crash A orphan fixture"),
			RecoveryWrite(FPaths::Combine(Orphan, TEXT("partial.tmp")), RecoveryBytes(TEXT("x"))));
		FRouteArtifactWorkspaceManager Manager;
		Recover(Root, ERouteArtifactRecoveryStatus::Recovered, Manager);
		FRouteArtifactManifest Loaded;
		TestTrue(TEXT("Crash A Current remains Ready"), LoadReady(Manager, Loaded));
		TestEqual(TEXT("Crash A Current identity"), Loaded.ArtifactSetID, A.ArtifactSetID);
		TestTrue(TEXT("Crash A Current bytes unchanged"),
			SameRecoveryTree(BeforeRecovery, CaptureRecoveryTree(Current)));
		TestFalse(TEXT("Crash A orphan removed"), IFileManager::Get().DirectoryExists(*Orphan));
		DeleteRecoveryRoot(Root);
	}

	// Crash B: even a READY orphan is never promoted.
	{
		const FString Root = RecoveryTempRoot(TEXT("CrashB"));
		FRouteArtifactManifest Artifact;
		if (!CreateReadyArtifact(*this, Root, TEXT("B"), 1, Artifact)) return false;
		const FString Current = FPaths::Combine(Root, TEXT("Current"));
		const FString Orphan = FPaths::Combine(Root, TEXT("Staging"), RecoveryAttemptID());
		TestTrue(TEXT("Crash B copy READY orphan"), CopyTree(Orphan, Current));
		TestTrue(TEXT("Crash B remove Current"),
			IFileManager::Get().DeleteDirectory(*Current, false, true));
		FRouteArtifactWorkspaceManager Manager;
		Recover(Root, ERouteArtifactRecoveryStatus::Recovered, Manager);
		TestFalse(TEXT("Crash B orphan not promoted"),
			IFileManager::Get().DirectoryExists(*Current));
		TestFalse(TEXT("Crash B orphan cleaned"), IFileManager::Get().DirectoryExists(*Orphan));
		DeleteRecoveryRoot(Root);
	}

	// Crash C: absent Current + exactly one valid previous restores previous, never attempt.
	{
		const FString Root = RecoveryTempRoot(TEXT("CrashC"));
		FRouteArtifactManifest A;
		if (!CreateReadyArtifact(*this, Root, TEXT("C-A"), 1, A)) return false;
		const FString Current = FPaths::Combine(Root, TEXT("Current"));
		const FString Previous = FPaths::Combine(
			Root, TEXT("Staging"), TEXT(".previous-") + RecoveryAttemptID());
		TestTrue(TEXT("Crash C Current to previous"), MoveTree(Previous, Current));
		const FString Orphan = FPaths::Combine(Root, TEXT("Staging"), RecoveryAttemptID());
		RecoveryWrite(FPaths::Combine(Orphan, TEXT("READY")), RecoveryBytes(TEXT("not-authority")));
		FRouteArtifactWorkspaceManager Manager;
		Recover(Root, ERouteArtifactRecoveryStatus::Recovered, Manager);
		FRouteArtifactManifest Loaded;
		TestTrue(TEXT("Crash C restored Current validates"), LoadReady(Manager, Loaded));
		TestEqual(TEXT("Crash C restored A"), Loaded.ArtifactSetID, A.ArtifactSetID);
		TestFalse(TEXT("Crash C attempt removed"), IFileManager::Get().DirectoryExists(*Orphan));
		DeleteRecoveryRoot(Root);
	}

	// Crash D: valid new Current stays; stale previous is removed.
	{
		const FString Root = RecoveryTempRoot(TEXT("CrashD"));
		FRouteArtifactManifest B;
		if (!CreateReadyArtifact(*this, Root, TEXT("D-B"), 1, B)) return false;
		const FString Current = FPaths::Combine(Root, TEXT("Current"));
		const FRecoveryTreeImage BeforeRecovery = CaptureRecoveryTree(Current);
		const FString Previous = FPaths::Combine(
			Root, TEXT("Staging"), TEXT(".previous-") + RecoveryAttemptID());
		TestTrue(TEXT("Crash D previous fixture"),
			CopyTree(Previous, Current));
		FRouteArtifactWorkspaceManager Manager;
		Recover(Root, ERouteArtifactRecoveryStatus::Recovered, Manager);
		FRouteArtifactManifest Loaded;
		TestTrue(TEXT("Crash D Current validates"), LoadReady(Manager, Loaded));
		TestEqual(TEXT("Crash D keeps B"), Loaded.ArtifactSetID, B.ArtifactSetID);
		TestTrue(TEXT("Crash D Current bytes unchanged"),
			SameRecoveryTree(BeforeRecovery, CaptureRecoveryTree(Current)));
		TestFalse(TEXT("Crash D previous cleaned"), IFileManager::Get().DirectoryExists(*Previous));
		DeleteRecoveryRoot(Root);
	}

	// Crash E: invalid Current is replaced by exactly one valid previous.
	{
		const FString Root = RecoveryTempRoot(TEXT("CrashE"));
		FRouteArtifactManifest A;
		if (!CreateReadyArtifact(*this, Root, TEXT("E-A"), 1, A)) return false;
		const FString Current = FPaths::Combine(Root, TEXT("Current"));
		const FString Previous = FPaths::Combine(
			Root, TEXT("Staging"), TEXT(".previous-") + RecoveryAttemptID());
		TestTrue(TEXT("Crash E previous fixture"), CopyTree(Previous, Current));
		RecoveryWrite(FPaths::Combine(Current, TEXT("manifest.json")), RecoveryBytes(TEXT("{}")));
		FRouteArtifactWorkspaceManager Manager;
		Recover(Root, ERouteArtifactRecoveryStatus::Recovered, Manager);
		FRouteArtifactManifest Loaded;
		TestTrue(TEXT("Crash E previous restored"), LoadReady(Manager, Loaded));
		TestEqual(TEXT("Crash E restored identity"), Loaded.ArtifactSetID, A.ArtifactSetID);
		DeleteRecoveryRoot(Root);
	}

	// Crash F: absent Current + invalid previous preserves evidence and requires recovery.
	{
		const FString Root = RecoveryTempRoot(TEXT("CrashF"));
		FRouteArtifactManifest A;
		if (!CreateReadyArtifact(*this, Root, TEXT("F-A"), 1, A)) return false;
		const FString Current = FPaths::Combine(Root, TEXT("Current"));
		const FString Previous = FPaths::Combine(
			Root, TEXT("Staging"), TEXT(".previous-") + RecoveryAttemptID());
		MoveTree(Previous, Current);
		RecoveryWrite(FPaths::Combine(Previous, TEXT("manifest.json")), RecoveryBytes(TEXT("{}")));
		FRouteArtifactWorkspaceManager Manager;
		Recover(Root, ERouteArtifactRecoveryStatus::RecoveryRequired, Manager);
		TestFalse(TEXT("Crash F does not invent Current"), IFileManager::Get().DirectoryExists(*Current));
		TestTrue(TEXT("Crash F preserves invalid previous"), IFileManager::Get().DirectoryExists(*Previous));
		DeleteRecoveryRoot(Root);
	}

	// Multiple valid previous candidates are ambiguous and preserved.
	{
		const FString Root = RecoveryTempRoot(TEXT("Multiple"));
		FRouteArtifactManifest A;
		if (!CreateReadyArtifact(*this, Root, TEXT("M-A"), 1, A)) return false;
		const FString Current = FPaths::Combine(Root, TEXT("Current"));
		const FString Previous1 = FPaths::Combine(
			Root, TEXT("Staging"), TEXT(".previous-") + RecoveryAttemptID());
		const FString Previous2 = FPaths::Combine(
			Root, TEXT("Staging"), TEXT(".previous-") + RecoveryAttemptID());
		MoveTree(Previous1, Current);
		CopyTree(Previous2, Previous1);
		FRouteArtifactWorkspaceManager Manager;
		Recover(Root, ERouteArtifactRecoveryStatus::RecoveryRequired, Manager);
		TestEqual(TEXT("Multiple previous code"),
			Manager.GetLastRecoveryResult().ErrorCode,
			ERouteArtifactRecoveryErrorCode::MultiplePreviousCandidates);
		TestFalse(TEXT("Multiple previous does not choose"), IFileManager::Get().DirectoryExists(*Current));
		TestTrue(TEXT("Previous 1 preserved"), IFileManager::Get().DirectoryExists(*Previous1));
		TestTrue(TEXT("Previous 2 preserved"), IFileManager::Get().DirectoryExists(*Previous2));
		DeleteRecoveryRoot(Root);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactRecoveryCleanupAndContentionTests,
	"DroneGCS.RouteArtifact.2C3B.CleanupAndContention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactRecoveryCleanupAndContentionTests::RunTest(const FString& Parameters)
{
	const FString Root = RecoveryTempRoot(TEXT("Cleanup"));
	FRouteArtifactManifest CurrentManifest;
	if (!CreateReadyArtifact(*this, Root, TEXT("cleanup-current"), 1, CurrentManifest)) return false;
	const FString Current = FPaths::Combine(Root, TEXT("Current"));
	const FString Orphan = FPaths::Combine(Root, TEXT("Staging"), RecoveryAttemptID());
	const FString Unknown = FPaths::Combine(Root, TEXT("Staging"), TEXT("user-unknown"));
	RecoveryWrite(FPaths::Combine(Orphan, TEXT("manifest.tmp")), RecoveryBytes(TEXT("partial")));
	RecoveryWrite(FPaths::Combine(Unknown, TEXT("keep.txt")), RecoveryBytes(TEXT("keep")));

	FRouteArtifactWorkspaceManager RecoveryManager;
	FString Error;
	TestTrue(TEXT("Unknown staging root accepted"),
		RecoveryManager.SetConfiguredWorkspaceRoot(Root, Error));
	TestEqual(TEXT("Unknown staging requires review"),
		RecoveryManager.GetLastRecoveryResult().Status,
		ERouteArtifactRecoveryStatus::RecoveryRequired);
	TestFalse(TEXT("Recognized orphan cleaned"), IFileManager::Get().DirectoryExists(*Orphan));
	TestTrue(TEXT("Unknown directory preserved"), IFileManager::Get().DirectoryExists(*Unknown));
	FRouteArtifactManifest Loaded;
	TestTrue(TEXT("Valid Current remains Ready with unknown staging"), LoadReady(RecoveryManager, Loaded));
	TestEqual(TEXT("Valid Current remains authoritative"),
		Loaded.ArtifactSetID, CurrentManifest.ArtifactSetID);

	// Remove only the explicit test unknown so generation can begin.
	IFileManager::Get().DeleteDirectory(*Unknown, false, true);
	FRouteArtifactRecoveryResult StableResult;
	TestTrue(TEXT("Explicit recovery becomes stable"),
		RecoveryManager.RecoverWorkspace(StableResult, Error));
	const FRouteArtifactBuildIdentity Build = RecoveryBuild(*this, TEXT("contention"));
	const FRouteArtifactWorkspaceAuthorityGate Gate = RecoveryGate(*this, Build, 1);
	FRouteArtifactGenerationHandle Handle;
	TestTrue(TEXT("Writer fixture Begin"),
		RecoveryManager.BeginGeneration(Build, Gate, Handle, Error));
	RecoveryWrite(FPaths::Combine(Handle.StagingRoot, TEXT("partial.tmp")), RecoveryBytes(TEXT("active")));

	FRouteArtifactWorkspaceManager Contender;
	TestFalse(TEXT("Recovery contender is Busy"),
		Contender.SetConfiguredWorkspaceRoot(Root, Error));
	TestEqual(TEXT("Recovery Busy status"),
		Contender.GetLastRecoveryResult().Status, ERouteArtifactRecoveryStatus::Busy);
	TestTrue(TEXT("Busy recovery does not delete active attempt"),
		IFileManager::Get().DirectoryExists(*Handle.StagingRoot));
	TestTrue(TEXT("Abort writer fixture"), RecoveryManager.AbortGeneration(Error));
	TestTrue(TEXT("Recovery allowed after writer release"),
		Contender.SetConfiguredWorkspaceRoot(Root, Error));

	DeleteRecoveryRoot(Root);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactCurrentAllowlistTests,
	"DroneGCS.RouteArtifact.2C3B.CurrentAllowlist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactCurrentAllowlistTests::RunTest(const FString& Parameters)
{
	auto ExpectExtraRejected = [this](const FString& Label, const FString& RelativeExtra)
	{
		const FString Root = RecoveryTempRoot(Label);
		FRouteArtifactManifest Manifest;
		if (!CreateReadyArtifact(*this, Root, Label, 3, Manifest)) return false;
		const FString Extra = FPaths::Combine(Root, TEXT("Current"), RelativeExtra);
		TestTrue(*FString::Printf(TEXT("%s extra fixture"), *Label),
			RecoveryWrite(Extra, RecoveryBytes(TEXT("extra"))));
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		TestTrue(*FString::Printf(TEXT("%s root configured"), *Label),
			Manager.SetConfiguredWorkspaceRoot(Root, Error));
		FRouteArtifactManifest Loaded;
		TestFalse(*FString::Printf(TEXT("%s Current rejected"), *Label),
			Manager.LoadAndValidateCurrentArtifact(Loaded, Error));
		TestEqual(*FString::Printf(TEXT("%s status Invalid"), *Label),
			Manager.GetCurrentStatus(), ERouteArtifactCurrentStatus::Invalid);
		TestTrue(*FString::Printf(TEXT("%s extra not auto-deleted"), *Label),
			IFileManager::Get().FileExists(*Extra));
		DeleteRecoveryRoot(Root);
		return true;
	};
	ExpectExtraRejected(TEXT("ExtraHighRoute"), TEXT("routes/00000000/4.waypoints"));
	ExpectExtraRejected(TEXT("UnknownTmp"), TEXT("unknown.tmp"));
	ExpectExtraRejected(TEXT("UnknownExe"), TEXT("random.exe"));
	ExpectExtraRejected(TEXT("OldManifest"), TEXT("old_manifest.json"));
	ExpectExtraRejected(TEXT("ManifestTmp"), TEXT("manifest.tmp"));
	ExpectExtraRejected(TEXT("ReadyTmp"), TEXT("READY.tmp"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactReparseSafetyTests,
	"DroneGCS.RouteArtifact.2C3B.ReparseSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactReparseSafetyTests::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS
	// Root itself as a directory symlink is rejected by the v1 policy.
	{
		const FString Base = RecoveryTempRoot(TEXT("RootReparse"));
		const FString Target = FPaths::Combine(Base, TEXT("target"));
		const FString Link = FPaths::Combine(Base, TEXT("workspace-link"));
		IFileManager::Get().MakeDirectory(*Target, true);
		TestTrue(TEXT("Create Root directory symlink"), CreateFilesystemLink(Link, Target, true));
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		TestFalse(TEXT("Configured Root reparse rejected"),
			Manager.SetConfiguredWorkspaceRoot(Link, Error));
		TestEqual(TEXT("Root reparse status"),
			Manager.GetLastRecoveryResult().Status, ERouteArtifactRecoveryStatus::Unrecoverable);
		RemoveFilesystemLink(Link, true);
		DeleteRecoveryRoot(Base);
	}

	// Current shard directory symlink to outside Root is rejected before route open.
	{
		const FString Root = RecoveryTempRoot(TEXT("DirectoryReparse"));
		const FString Outside = RecoveryTempRoot(TEXT("OutsideDirectory"));
		FRouteArtifactManifest Manifest;
		if (!CreateReadyArtifact(*this, Root, TEXT("dir-reparse"), 1, Manifest)) return false;
		const FString Shard = FPaths::Combine(Root, TEXT("Current/routes/00000000"));
		IFileManager::Get().MakeDirectory(*Outside, true);
		CopyTree(Outside, Shard);
		IFileManager::Get().DeleteDirectory(*Shard, false, true);
		TestTrue(TEXT("Create shard directory symlink"), CreateFilesystemLink(Shard, Outside, true));
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		TestFalse(TEXT("Directory reparse root configuration rejected"),
			Manager.SetConfiguredWorkspaceRoot(Root, Error));
		RemoveFilesystemLink(Shard, true);
		DeleteRecoveryRoot(Root);
		DeleteRecoveryRoot(Outside);
	}

	// Route file symlink to an outside file is rejected.
	{
		const FString Root = RecoveryTempRoot(TEXT("FileReparse"));
		const FString OutsideRoot = RecoveryTempRoot(TEXT("OutsideFile"));
		FRouteArtifactManifest Manifest;
		if (!CreateReadyArtifact(*this, Root, TEXT("file-reparse"), 1, Manifest)) return false;
		const FString Route = FPaths::Combine(Root, TEXT("Current"), Manifest.Routes[0].RelativePath);
		const FString Outside = FPaths::Combine(OutsideRoot, TEXT("outside.waypoints"));
		TArray<uint8> Original;
		FFileHelper::LoadFileToArray(Original, *Route);
		RecoveryWrite(Outside, Original);
		IFileManager::Get().Delete(*Route, false, true, true);
		TestTrue(TEXT("Create route file symlink"), CreateFilesystemLink(Route, Outside, false));
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		TestFalse(TEXT("Route file reparse root configuration rejected"),
			Manager.SetConfiguredWorkspaceRoot(Root, Error));
		RemoveFilesystemLink(Route, false);
		DeleteRecoveryRoot(Root);
		DeleteRecoveryRoot(OutsideRoot);
	}

	// Safe staging resolver refuses a shard switched to an outside symlink.
	{
		const FString Root = RecoveryTempRoot(TEXT("StagingReparse"));
		const FString Outside = RecoveryTempRoot(TEXT("OutsideStaging"));
		FRouteArtifactWorkspaceManager Manager;
		FString Error;
		TestTrue(TEXT("Staging Root configured"), Manager.SetConfiguredWorkspaceRoot(Root, Error));
		const FRouteArtifactBuildIdentity Build = RecoveryBuild(*this, TEXT("staging-reparse"));
		const FRouteArtifactWorkspaceAuthorityGate Gate = RecoveryGate(*this, Build, 1);
		FRouteArtifactGenerationHandle Handle;
		TestTrue(TEXT("Staging reparse Begin"), Manager.BeginGeneration(Build, Gate, Handle, Error));
		FString RoutePath;
		TestTrue(TEXT("Initial safe staging resolve"),
			Manager.ResolveSafeStagingRoutePath(0, RoutePath, Error));
		const FString Shard = FPaths::GetPath(RoutePath);
		IFileManager::Get().DeleteDirectory(*Shard, false, true);
		IFileManager::Get().MakeDirectory(*Outside, true);
		TestTrue(TEXT("Create staging shard symlink"), CreateFilesystemLink(Shard, Outside, true));
		TestFalse(TEXT("Safe staging resolver rejects escape"),
			Manager.ResolveSafeStagingRoutePath(0, RoutePath, Error));
		RemoveFilesystemLink(Shard, true);
		TestTrue(TEXT("Abort staging reparse fixture"), Manager.AbortGeneration(Error));
		DeleteRecoveryRoot(Root);
		DeleteRecoveryRoot(Outside);
	}
#else
	AddWarning(TEXT("Reparse safety fixtures are Windows-only."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactTenThousandAllowlistTests,
	"DroneGCS.RouteArtifact.2C3B.TenThousandAllowlist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactTenThousandAllowlistTests::RunTest(const FString& Parameters)
{
	FRouteArtifactManifest Manifest;
	Manifest.Routes.Reserve(10000);
	TArray<FString> Files{ TEXT("manifest.json"), TEXT("READY") };
	Files.Reserve(10002);
	TSet<FString> DirectorySet;
	FString Error;
	for (int32 Slot = 0; Slot < 10000; ++Slot)
	{
		FRouteArtifactRecord& Record = Manifest.Routes.AddDefaulted_GetRef();
		Record.RouteSlotIndex = Slot;
		TestTrue(TEXT("10K relative path"), RouteArtifactWorkspacePaths::BuildRouteRelativePath(
			Slot, Record.RelativePath, Error));
		Files.Add(Record.RelativePath);
		DirectorySet.Add(TEXT("routes"));
		DirectorySet.Add(FPaths::GetPath(Record.RelativePath));
	}
	TArray<FString> Directories = DirectorySet.Array();
	TestTrue(TEXT("10K synthetic allowlist accepted"),
		FRouteArtifactWorkspaceManager::ValidateCurrentEntryAllowlistForTest(
			Manifest, Files, Directories, Error));
	Files.Add(TEXT("routes/00000000/10000.waypoints"));
	TestFalse(TEXT("10K synthetic allowlist rejects extra"),
		FRouteArtifactWorkspaceManager::ValidateCurrentEntryAllowlistForTest(
			Manifest, Files, Directories, Error));
	return true;
}

#endif
