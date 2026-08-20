#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DSPGCSFrozenAssignmentSyncSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceManager.h"
#include "RouteArtifactWorkspacePaths.h"
#include "RouteArtifactWorkspaceSubsystem.h"
#include "RouteAssignmentSubsystem.h"
#include "RoutePreviewSubsystem.h"
#include "RouteWaypointGenerationSubsystem.h"

namespace
{
	FRouteArtifactDigest256 V2Hash(
		FAutomationTestBase& Test,
		const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FRouteArtifactDigest256 Digest;
		FString Error;
		Test.TestTrue(TEXT("Synthetic SHA-256"), FRouteArtifactSha256::HashBytes(
			Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest, Error));
		return Digest;
	}

	FString V2TempRoot(const FString& Label)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("RouteArtifactV2BTests"),
			Label + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	FRouteArtifactSourceIdentity V2Source(
		FAutomationTestBase& Test,
		const FRouteArtifactDigest256& Digest)
	{
		FRouteArtifactSourceIdentity Source;
		FString Error;
		Test.TestTrue(TEXT("v2 source identity"), FRouteArtifactSourceIdentity::Create(
			Digest, 1234, TEXT("offline-show.npz"), Source, Error));
		return Source;
	}

	FRouteArtifactBuildIdentityV2 V2Build(FAutomationTestBase& Test)
	{
		FRouteArtifactBuildIdentityV2 Build;
		Build.SourceDatasetDigest = V2Hash(Test, TEXT("offline-source"));
		Build.WaypointGeneratorVersion =
			RouteWaypointGeneratorContract::WaypointGeneratorVersion;
		Build.GeneratorInputDigest = V2Hash(Test, TEXT("offline-generator-input"));
		Build.ShowOriginLatitudeE7 = 305000000;
		Build.ShowOriginLongitudeE7 = 1202500000;
		Build.FrameCount = 2;
		Build.RouteSlotCount = 1;
		return Build;
	}

	FRouteArtifactWorkspaceSourceGate V2Gate(
		FAutomationTestBase& Test,
		const FRouteArtifactBuildIdentityV2& Build,
		const int64 RouteDatasetGeneration = 1)
	{
		FRouteArtifactWorkspaceSourceGate Gate;
		Gate.bSourceExists = true;
		Gate.ActiveSourceIdentity = V2Source(Test, Build.SourceDatasetDigest);
		Gate.RouteDatasetGeneration = RouteDatasetGeneration;
		Gate.FrameCount = Build.FrameCount;
		Gate.RouteSlotCount = Build.RouteSlotCount;
		Gate.bHasRange = Build.bHasRange;
		Gate.RangeStart = Build.RangeStart;
		Gate.RangeEnd = Build.RangeEnd;
		Gate.ShowOriginLatitudeE7 = Build.ShowOriginLatitudeE7;
		Gate.ShowOriginLongitudeE7 = Build.ShowOriginLongitudeE7;
		Gate.WaypointGeneratorVersion = Build.WaypointGeneratorVersion;
		Gate.GeneratorInputDigest = Build.GeneratorInputDigest;
		return Gate;
	}

	TArray<uint8> V2TextBytes(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	bool WriteV2Draft(
		FAutomationTestBase& Test,
		FRouteArtifactWorkspaceManager& Manager,
		const FRouteArtifactBuildIdentityV2& Build,
		const FRouteArtifactWorkspaceSourceGate& Gate,
		const FRouteArtifactGenerationHandle& Handle,
		const int64 CreatedAtUtc,
		FRouteArtifactManifestV2& OutManifest)
	{
		OutManifest = {};
		OutManifest.ArtifactSetID = Handle.ArtifactSetID;
		OutManifest.CreatedAtUtc = CreatedAtUtc;
		OutManifest.SourceIdentity = Gate.ActiveSourceIdentity;
		OutManifest.FrameCount = Build.FrameCount;
		OutManifest.RouteSlotCount = Build.RouteSlotCount;
		OutManifest.bHasRange = Build.bHasRange;
		OutManifest.RangeStart = Build.RangeStart;
		OutManifest.RangeEnd = Build.RangeEnd;
		OutManifest.ShowOriginLatitudeE7 = Build.ShowOriginLatitudeE7;
		OutManifest.ShowOriginLongitudeE7 = Build.ShowOriginLongitudeE7;
		OutManifest.WaypointGeneratorVersion = Build.WaypointGeneratorVersion;
		OutManifest.GeneratorInputDigest = Build.GeneratorInputDigest;

		FString Error;
		FString RoutePath;
		if (!Manager.ResolveSafeStagingRoutePath(0, RoutePath, Error))
		{
			Test.AddError(Error);
			return false;
		}
		const TArray<uint8> Bytes = V2TextBytes(
			TEXT("QGC WPL 110\r\n")
			TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1\r\n")
			TEXT("1\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(RoutePath), true);
		if (!FFileHelper::SaveArrayToFile(Bytes, *RoutePath))
		{
			Test.AddError(TEXT("Could not write v2 route fixture."));
			return false;
		}
		FRouteArtifactRecord& Record = OutManifest.Routes.AddDefaulted_GetRef();
		Record.RouteSlotIndex = 0;
		if (!RouteArtifactWorkspacePaths::BuildRouteRelativePath(
			0, Record.RelativePath, Error)
			|| !FRouteArtifactSha256::HashFileStreaming(
				RoutePath, Record.RouteArtifactDigest, Record.FileByteSize, Error)
			|| !RouteArtifactManifestV2::ComputeArtifactSetDigest(
				OutManifest, OutManifest.ArtifactSetDigest, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return true;
	}

	using FDirectoryImageV2 = TMap<FString, TArray<uint8>>;

	FDirectoryImageV2 CaptureV2Directory(const FString& Root)
	{
		FDirectoryImageV2 Image;
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

	bool V2ImagesEqual(
		const FDirectoryImageV2& Left,
		const FDirectoryImageV2& Right)
	{
		if (Left.Num() != Right.Num()) return false;
		for (const TPair<FString, TArray<uint8>>& Entry : Left)
		{
			const TArray<uint8>* Other = Right.Find(Entry.Key);
			if (!Other || *Other != Entry.Value) return false;
		}
		return true;
	}

	FRouteArtifactBuildIdentity V1Build(FAutomationTestBase& Test)
	{
		FRouteArtifactBuildIdentity Build;
		Build.AssignmentBinding.FleetSessionID = TEXT("legacy-fleet");
		Build.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("legacy-authority");
		Build.AssignmentBinding.AssignmentRevision = 1;
		Build.AssignmentBinding.AssignmentDigest = V2Hash(Test, TEXT("legacy-assignment"));
		Build.SourceDatasetDigest = V2Hash(Test, TEXT("legacy-source"));
		Build.WaypointGeneratorVersion = TEXT("qgc-wpl110-wgs84-relative-rgb-v1");
		Build.GeneratorInputDigest = V2Hash(Test, TEXT("legacy-generator"));
		return Build;
	}

	FRouteArtifactWorkspaceAuthorityGate V1Gate(
		FAutomationTestBase& Test,
		const FRouteArtifactBuildIdentity& Build)
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
		Gate.ActiveSourceIdentity = V2Source(Test, Build.SourceDatasetDigest);
		Gate.RouteSlotCount = 1;
		return Gate;
	}

	bool CreateV1Current(
		FAutomationTestBase& Test,
		FRouteArtifactWorkspaceManager& Manager,
		const FString& Root)
	{
		FString Error;
		if (!Manager.SetConfiguredWorkspaceRoot(Root, Error))
		{
			Test.AddError(Error);
			return false;
		}
		const FRouteArtifactBuildIdentity Build = V1Build(Test);
		const FRouteArtifactWorkspaceAuthorityGate Gate = V1Gate(Test, Build);
		FRouteArtifactGenerationHandle Handle;
		if (!Manager.BeginGeneration(Build, Gate, Handle, Error))
		{
			Test.AddError(Error);
			return false;
		}
		FString RoutePath;
		if (!Manager.ResolveSafeStagingRoutePath(0, RoutePath, Error))
		{
			Test.AddError(Error);
			return false;
		}
		const TArray<uint8> Bytes = V2TextBytes(
			TEXT("QGC WPL 110\r\n")
			TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1\r\n")
			TEXT("1\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(RoutePath), true);
		if (!FFileHelper::SaveArrayToFile(Bytes, *RoutePath)) return false;
		FRouteArtifactManifest Manifest;
		Manifest.ArtifactSetID = Handle.ArtifactSetID;
		Manifest.CreatedAtUtc = 1700000000000;
		Manifest.AssignmentBinding = Build.AssignmentBinding;
		Manifest.FleetRevisionAtCapture = 1;
		Manifest.RouteDatasetGeneration = 1;
		Manifest.CompletionMode = ERouteAssignmentCompletionMode::Full;
		Manifest.FreezePolicy = ERouteAssignmentFreezePolicy::RequireFull;
		Manifest.MatchedCount = 1;
		Manifest.SourceIdentity = Gate.ActiveSourceIdentity;
		Manifest.FrameCount = 2;
		Manifest.RouteSlotCount = 1;
		Manifest.WaypointGeneratorVersion = Build.WaypointGeneratorVersion;
		Manifest.GeneratorInputDigest = Build.GeneratorInputDigest;
		Manifest.MatchedMapping.Add({TEXT("legacy-drone"), 0});
		FRouteArtifactRecord& Record = Manifest.Routes.AddDefaulted_GetRef();
		Record.RouteSlotIndex = 0;
		if (!RouteArtifactWorkspacePaths::BuildRouteRelativePath(
			0, Record.RelativePath, Error)
			|| !FRouteArtifactSha256::HashFileStreaming(
				RoutePath, Record.RouteArtifactDigest, Record.FileByteSize, Error)
			|| !RouteArtifactManifest::ComputeArtifactSetDigest(
				Manifest, Manifest.ArtifactSetDigest, Error)
			|| !Manager.FinalizeGeneration(Manifest, Gate, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return true;
	}

	bool DecodeOfflineRoute(TArray<uint8>& OutBytes)
	{
		static const FString Encoded = TEXT(
			"UEsDBC0AAAAAAAAAIQDCWHD8//////////8MABQAcG9zaXRpb24ubnB5AQAQAIwAAAAAAAAAjAAAAAAAAACTTlVNUFkBAHYAeydkZXNjcic6ICc8ZjQnLCAnZm9ydHJhbl9vcmRlcic6IEZhbHNlLCAnc2hhcGUnOiAoMSwgMyksIH0gICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgCgAAAAAAAAAAAAAAAFBLAwQtAAAAAAAAACEAMNa+VP//////////CQAUAGNvbG9yLm5weQEAEACQAAAAAAAAAJAAAAAAAAAAk05VTVBZAQB2AHsnZGVzY3InOiAnPGY0JywgJ2ZvcnRyYW5fb3JkZXInOiBGYWxzZSwgJ3NoYXBlJzogKDEsIDQpLCB9ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIAoAAIA/AACAPwAAgD8AAIA/UEsBAi0ALQAAAAAAAAAhAMJYcPyMAAAAjAAAAAwAAAAAAAAAAAAAAIABAAAAAHBvc2l0aW9uLm5weVBLAQItAC0AAAAAAAAAIQAw1r5UkAAAAJAAAAAJAAAAAAAAAAAAAACAAcoAAABjb2xvci5ucHlQSwUGAAAAAAIAAgBxAAAAlQEAAAAA");
		return FBase64::Decode(Encoded, OutBytes);
	}

	struct FOfflineGenerationState
	{
		FAutomationTestBase* Test = nullptr;
		UGameInstance* GameInstance = nullptr;
		URouteWaypointGenerationSubsystem* Generator = nullptr;
		URouteArtifactWorkspaceSubsystem* Workspace = nullptr;
		FString RouteFile;
		FString WorkspaceRoot;
		double DeadlineSeconds = 0.0;

		void Cleanup()
		{
			if (GameInstance)
			{
				GameInstance->Shutdown();
				GameInstance->RemoveFromRoot();
				GameInstance = nullptr;
			}
			IFileManager::Get().Delete(*RouteFile, false, true, true);
			IFileManager::Get().DeleteDirectory(*WorkspaceRoot, false, true);
		}
	};

	class FWaitForOfflineV2Generation final : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForOfflineV2Generation(TSharedRef<FOfflineGenerationState> InState)
			: State(MoveTemp(InState))
		{
		}

		virtual bool Update() override
		{
			if (!State->Generator->IsGenerationActive())
			{
				const FRouteWaypointBatchProgress Progress = State->Generator->GetProgress();
				State->Test->TestEqual(TEXT("Offline generation succeeded"),
					Progress.GenerationState, ERouteWaypointBatchState::Succeeded);
				State->Test->TestEqual(TEXT("Current is READY v2"),
					State->Workspace->GetCurrentStatus(), ERouteArtifactCurrentStatus::Ready);
				State->Test->TestEqual(TEXT("Current schema is v2"),
					State->Workspace->GetCurrentInfo().ArtifactSchemaVersion,
					RouteArtifactContractV2::ArtifactSchemaVersion);
				FRouteArtifactManifestV2 Manifest;
				FString Error;
				State->Test->TestTrue(TEXT("Production Current v2 validates"),
					State->Workspace->LoadAndValidateCurrentArtifact(Manifest, Error));
				State->Test->TestEqual(TEXT("Production manifest schema v2"),
					Manifest.ArtifactSchemaVersion,
					RouteArtifactContractV2::ArtifactSchemaVersion);
				State->Cleanup();
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				FString CancelError;
				State->Generator->CancelGeneration(CancelError);
				State->Test->AddError(TEXT("Offline production generation exceeded 30 second deadline."));
				State->Cleanup();
				return true;
			}
			return false;
		}

	private:
		TSharedRef<FOfflineGenerationState> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactV2WorkspaceSourceSafetyTest,
	"DroneGCS.RouteArtifactV2B.WorkspaceSourceSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactV2WorkspaceSourceSafetyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Root = V2TempRoot(TEXT("SourceSafety"));
	FRouteArtifactWorkspaceManager Manager;
	FString Error;
	TestTrue(TEXT("v2 root configured"), Manager.SetConfiguredWorkspaceRoot(Root, Error));
	const FRouteArtifactBuildIdentityV2 Build = V2Build(*this);
	const FRouteArtifactWorkspaceSourceGate Gate = V2Gate(*this, Build);
	FRouteArtifactBuildIdentityV2 LegacyGeneratorBuild = Build;
	LegacyGeneratorBuild.WaypointGeneratorVersion =
		TEXT("qgc-wpl110-wgs84-relative-rgb-v1");
	FString LegacyGeneratorSetID;
	FString SaturatingGeneratorSetID;
	TestTrue(TEXT("legacy generator v2 identity"),
		RouteArtifactIdentityV2::ComputeArtifactSetID(
			LegacyGeneratorBuild, LegacyGeneratorSetID, Error));
	TestTrue(TEXT("saturating generator v2 identity"),
		RouteArtifactIdentityV2::ComputeArtifactSetID(
			Build, SaturatingGeneratorSetID, Error));
	TestNotEqual(TEXT("GeneratorVersion changes v2 ArtifactSetID"),
		LegacyGeneratorSetID, SaturatingGeneratorSetID);
	FRouteArtifactAssignmentBinding Fleet0Assignment;
	Fleet0Assignment.FleetSessionID = TEXT("fleet-0");
	Fleet0Assignment.AssignmentRevision = 1;
	FRouteArtifactAssignmentBinding Fleet1000Assignment;
	Fleet1000Assignment.FleetSessionID = TEXT("fleet-1000");
	Fleet1000Assignment.AssignmentRevision = 999;
	Fleet1000Assignment.AssignmentDigest = V2Hash(*this, TEXT("different-assignment"));
	TestTrue(TEXT("external Fleet/Assignment fixtures differ"),
		Fleet0Assignment.FleetSessionID != Fleet1000Assignment.FleetSessionID
		&& Fleet0Assignment.AssignmentRevision != Fleet1000Assignment.AssignmentRevision);
	FString Fleet0SetID;
	FString Fleet1000SetID;
	TestTrue(TEXT("Fleet 0 v2 identity"),
		RouteArtifactIdentityV2::ComputeArtifactSetID(Build, Fleet0SetID, Error));
	TestTrue(TEXT("Fleet 1000 v2 identity"),
		RouteArtifactIdentityV2::ComputeArtifactSetID(Build, Fleet1000SetID, Error));
	TestEqual(TEXT("Fleet/Assignment context cannot change v2 ArtifactSetID"),
		Fleet0SetID, Fleet1000SetID);
	FRouteArtifactGenerationHandle Handle;
	TestTrue(TEXT("pure-route v2 Begin"), Manager.BeginGeneration(Build, Gate, Handle, Error));
	FRouteArtifactManifestV2 Manifest;
	if (!WriteV2Draft(*this, Manager, Build, Gate, Handle, 1700000000000, Manifest))
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	TestTrue(TEXT("pure-route v2 Finalize"), Manager.FinalizeGeneration(Manifest, Gate, Error));
	TestEqual(TEXT("v2 Current Ready"), Manager.GetCurrentStatus(),
		ERouteArtifactCurrentStatus::Ready);
	const FString Current = FPaths::Combine(Root, TEXT("Current"));
	const FDirectoryImageV2 Baseline = CaptureV2Directory(Current);

	FRouteArtifactGenerationHandle StaleHandle;
	TestTrue(TEXT("second pure-route Begin"),
		Manager.BeginGeneration(Build, Gate, StaleHandle, Error));
	FRouteArtifactManifestV2 StaleManifest;
	WriteV2Draft(*this, Manager, Build, Gate, StaleHandle, 1700000000001, StaleManifest);
	FRouteArtifactWorkspaceSourceGate ChangedSource = Gate;
	++ChangedSource.RouteDatasetGeneration;
	TestFalse(TEXT("Route source generation change rejects promotion"),
		Manager.FinalizeGeneration(StaleManifest, ChangedSource, Error));
	TestTrue(TEXT("Source change preserves old Current"),
		V2ImagesEqual(CaptureV2Directory(Current), Baseline));

	FRouteArtifactGenerationHandle OriginHandle;
	TestTrue(TEXT("third pure-route Begin"),
		Manager.BeginGeneration(Build, Gate, OriginHandle, Error));
	FRouteArtifactManifestV2 OriginManifest;
	WriteV2Draft(*this, Manager, Build, Gate, OriginHandle, 1700000000002, OriginManifest);
	FRouteArtifactWorkspaceSourceGate ChangedOrigin = Gate;
	++ChangedOrigin.ShowOriginLatitudeE7;
	TestFalse(TEXT("ShowOrigin change rejects promotion"),
		Manager.FinalizeGeneration(OriginManifest, ChangedOrigin, Error));
	TestTrue(TEXT("Origin change preserves old Current"),
		V2ImagesEqual(CaptureV2Directory(Current), Baseline));
	Manager.Shutdown();
	FRouteArtifactWorkspaceManager RecoveryManager;
	TestTrue(TEXT("v2 Current recovery"),
		RecoveryManager.SetConfiguredWorkspaceRoot(Root, Error));
	TestEqual(TEXT("recovered v2 Current stays Ready"),
		RecoveryManager.GetCurrentStatus(), ERouteArtifactCurrentStatus::Ready);
	TestEqual(TEXT("recovered v2 Current schema"),
		RecoveryManager.GetCurrentInfo().ArtifactSchemaVersion,
		RouteArtifactContractV2::ArtifactSchemaVersion);
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactV2LegacyReplacementTest,
	"DroneGCS.RouteArtifactV2B.LegacyReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactV2LegacyReplacementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Root = V2TempRoot(TEXT("LegacyReplacement"));
	FRouteArtifactWorkspaceManager LegacyWriter;
	if (!CreateV1Current(*this, LegacyWriter, Root)) return false;
	LegacyWriter.Shutdown();
	const FString Current = FPaths::Combine(Root, TEXT("Current"));
	const FDirectoryImageV2 LegacyImage = CaptureV2Directory(Current);

	FRouteArtifactWorkspaceManager Manager;
	FString Error;
	TestTrue(TEXT("valid v1 Current does not block recovery"),
		Manager.SetConfiguredWorkspaceRoot(Root, Error));
	TestEqual(TEXT("v1 Current is explicit LegacyUnsupported"),
		Manager.GetCurrentStatus(), ERouteArtifactCurrentStatus::LegacyUnsupported);

	const FRouteArtifactBuildIdentityV2 Build = V2Build(*this);
	const FRouteArtifactWorkspaceSourceGate Gate = V2Gate(*this, Build);
	FRouteArtifactGenerationHandle FailedHandle;
	TestTrue(TEXT("v2 Begin allowed over legacy Current"),
		Manager.BeginGeneration(Build, Gate, FailedHandle, Error));
	FRouteArtifactManifestV2 FailedManifest;
	WriteV2Draft(*this, Manager, Build, Gate, FailedHandle, 1700000000100, FailedManifest);
	FRouteArtifactWorkspaceSourceGate StaleGate = Gate;
	++StaleGate.RouteDatasetGeneration;
	TestFalse(TEXT("failed v2 replacement rejected"),
		Manager.FinalizeGeneration(FailedManifest, StaleGate, Error));
	TestTrue(TEXT("failed v2 replacement preserves v1 Current"),
		V2ImagesEqual(CaptureV2Directory(Current), LegacyImage));

	FRouteArtifactGenerationHandle SuccessHandle;
	TestTrue(TEXT("successful v2 replacement Begin"),
		Manager.BeginGeneration(Build, Gate, SuccessHandle, Error));
	FRouteArtifactManifestV2 SuccessManifest;
	WriteV2Draft(*this, Manager, Build, Gate, SuccessHandle, 1700000000200, SuccessManifest);
	TestTrue(TEXT("successful v2 replacement Finalize"),
		Manager.FinalizeGeneration(SuccessManifest, Gate, Error));
	FRouteArtifactManifestV2 LoadedV2;
	TestTrue(TEXT("Current is now valid v2"),
		Manager.LoadAndValidateCurrentArtifact(LoadedV2, Error));
	TestEqual(TEXT("replacement schema"), LoadedV2.ArtifactSchemaVersion,
		RouteArtifactContractV2::ArtifactSchemaVersion);
	TestFalse(TEXT("sets archive not created"),
		IFileManager::Get().DirectoryExists(*FPaths::Combine(Root, TEXT("sets"))));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactV2OfflineProductionGenerationTest,
	"DroneGCS.RouteArtifactV2B.OfflineProductionGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactV2OfflineProductionGenerationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<uint8> RouteBytes;
	if (!TestTrue(TEXT("offline NPZ fixture decodes"), DecodeOfflineRoute(RouteBytes)))
	{
		return false;
	}
	TSharedRef<FOfflineGenerationState> State = MakeShared<FOfflineGenerationState>();
	State->Test = this;
	State->WorkspaceRoot = V2TempRoot(TEXT("OfflineProduction"));
	State->RouteFile = FPaths::Combine(
		FPaths::GetPath(State->WorkspaceRoot),
		TEXT("offline-route-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ TEXT(".npz"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(State->RouteFile), true);
	if (!TestTrue(TEXT("offline NPZ fixture written"),
		FFileHelper::SaveArrayToFile(RouteBytes, *State->RouteFile)))
	{
		return false;
	}

	State->GameInstance = NewObject<UGameInstance>(GEngine);
	State->GameInstance->AddToRoot();
	State->GameInstance->InitializeStandalone(TEXT("RouteArtifactV2BOfflineGeneration"));
	UFleetManagerSubsystem* Fleet =
		State->GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	URouteAssignmentSubsystem* Assignment =
		State->GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	UDSPGCSFrozenAssignmentSyncSubsystem* Sync =
		State->GameInstance->GetSubsystem<UDSPGCSFrozenAssignmentSyncSubsystem>();
	URoutePreviewSubsystem* Preview =
		State->GameInstance->GetSubsystem<URoutePreviewSubsystem>();
	State->Workspace =
		State->GameInstance->GetSubsystem<URouteArtifactWorkspaceSubsystem>();
	State->Generator =
		State->GameInstance->GetSubsystem<URouteWaypointGenerationSubsystem>();
	if (!TestNotNull(TEXT("Fleet subsystem"), Fleet)
		|| !TestNotNull(TEXT("Assignment subsystem"), Assignment)
		|| !TestNotNull(TEXT("Frozen sync subsystem"), Sync)
		|| !TestNotNull(TEXT("Route Preview subsystem"), Preview)
		|| !TestNotNull(TEXT("Workspace subsystem"), State->Workspace)
		|| !TestNotNull(TEXT("Generation subsystem"), State->Generator))
	{
		State->Cleanup();
		return false;
	}
	TestEqual(TEXT("Fleet has zero drones"), Fleet->GetDroneCount(), 0);
	TestEqual(TEXT("Assignment is Empty"), Assignment->GetState(),
		ERouteAssignmentState::Empty);
	TestTrue(TEXT("Console is not accepted"),
		Sync->GetSyncState() != EDSPGCSFrozenAssignmentSyncState::ConsoleAccepted);

	FString Error;
	if (!TestTrue(TEXT("offline Workspace root configured"),
		State->Workspace->SetConfiguredWorkspaceRoot(State->WorkspaceRoot, Error))
		|| !TestTrue(TEXT("offline route prepared"),
			Preview->PrepareRouteImport(State->RouteFile, Error))
		|| !TestTrue(TEXT("offline route committed"),
			Preview->CommitPreparedRoute(Error))
		|| !TestTrue(TEXT("offline ShowOrigin set"),
			Preview->SetShowOrigin(30.5, 120.25, 50.0, Error))
		|| !TestTrue(TEXT("StartGeneration accepted without Fleet/Assignment/Console"),
			State->Generator->StartGeneration(1, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		State->Cleanup();
		return false;
	}
	State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForOfflineV2Generation(State));
	return true;
}

#endif
