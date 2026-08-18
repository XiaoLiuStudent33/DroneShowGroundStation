#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RouteArtifactTypes.h"
#include "RoutePreviewSubsystem.h"

namespace
{
	bool DecodeEmbeddedRoute(TArray<uint8>& OutBytes)
	{
		static const FString Encoded = TEXT(
			"UEsDBC0AAAAAAAAAIQDCWHD8//////////8MABQAcG9zaXRpb24ubnB5AQAQAIwAAAAAAAAAjAAAAAAAAACTTlVNUFkBAHYAeydkZXNjcic6ICc8ZjQnLCAnZm9ydHJhbl9vcmRlcic6IEZhbHNlLCAnc2hhcGUnOiAoMSwgMyksIH0gICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgCgAAAAAAAAAAAAAAAFBLAwQtAAAAAAAAACEAMNa+VP//////////CQAUAGNvbG9yLm5weQEAEACQAAAAAAAAAJAAAAAAAAAAk05VTVBZAQB2AHsnZGVzY3InOiAnPGY0JywgJ2ZvcnRyYW5fb3JkZXInOiBGYWxzZSwgJ3NoYXBlJzogKDEsIDQpLCB9ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIAoAAIA/AACAPwAAgD8AAIA/UEsBAi0ALQAAAAAAAAAhAMJYcPyMAAAAjAAAAAwAAAAAAAAAAAAAAIABAAAAAHBvc2l0aW9uLm5weVBLAQItAC0AAAAAAAAAIQAw1r5UkAAAAJAAAAAJAAAAAAAAAAAAAACAAcoAAABjb2xvci5ucHlQSwUGAAAAAAIAAgBxAAAAlQEAAAAA");
		return FBase64::Decode(Encoded, OutBytes);
	}

	bool SameSourceIdentity(
		const FRouteArtifactSourceIdentity& Left,
		const FRouteArtifactSourceIdentity& Right)
	{
		return Left.SourceDatasetDigest == Right.SourceDatasetDigest
			&& Left.SourceByteSize == Right.SourceByteSize
			&& Left.SourceDisplayFileName == Right.SourceDisplayFileName;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactSourceIdentityLifecycleTest,
	"DroneGCS.RouteArtifact.05SourceIdentityLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactSourceIdentityLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<uint8> RouteBytes;
	if (!TestTrue(TEXT("Embedded NPZ decodes"), DecodeEmbeddedRoute(RouteBytes)))
	{
		return false;
	}
	const FString Directory = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RouteArtifactSourceIdentity"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString RouteA = FPaths::Combine(Directory, TEXT("Route-A.npz"));
	const FString RouteB = FPaths::Combine(Directory, TEXT("Route-B.npz"));
	const FString BrokenRoute = FPaths::Combine(Directory, TEXT("Broken.npz"));
	const TArray<uint8> BrokenBytes{ 1, 2, 3, 4 };
	if (!TestTrue(TEXT("Route A written"), FFileHelper::SaveArrayToFile(RouteBytes, *RouteA))
		|| !TestTrue(TEXT("Route B written"), FFileHelper::SaveArrayToFile(RouteBytes, *RouteB))
		|| !TestTrue(TEXT("Broken route written"), FFileHelper::SaveArrayToFile(BrokenBytes, *BrokenRoute)))
	{
		return false;
	}

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteArtifactSourceIdentityTestWorld"));
	URoutePreviewSubsystem* Route = GameInstance->GetSubsystem<URoutePreviewSubsystem>();
	if (!TestNotNull(TEXT("Route Preview subsystem exists"), Route))
	{
		GameInstance->Shutdown();
		return false;
	}

	FString Error;
	TestEqual(TEXT("Initial generation"), Route->GetRouteDatasetGeneration(), static_cast<int64>(0));
	TestFalse(TEXT("Initial active source is empty"), Route->GetActiveSourceIdentity().IsValid());
	TestTrue(TEXT("Prepare route A succeeds"), Route->PrepareRouteImport(RouteA, Error));
	TestEqual(TEXT("Prepare does not increment generation"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(0));
	const FRouteArtifactSourceIdentity PreparedA = Route->GetPreparedSourceIdentity();
	TestTrue(TEXT("Prepared route has source identity"), PreparedA.IsValid());
	TestEqual(TEXT("Prepared route keeps basename only"),
		PreparedA.SourceDisplayFileName, FString(TEXT("Route-A.npz")));
	TestEqual(TEXT("Prepared route byte size"),
		PreparedA.SourceByteSize, static_cast<int64>(RouteBytes.Num()));
	TArray<uint8> ChangedRouteBytes = RouteBytes;
	ChangedRouteBytes[0] ^= 1;
	FRouteArtifactDigest256 ChangedSourceDigest;
	TestTrue(TEXT("Changed source bytes hash"),
		FRouteArtifactSha256::HashBytes(ChangedRouteBytes, ChangedSourceDigest, Error));
	TestTrue(TEXT("Different source bytes have different digest"),
		ChangedSourceDigest != PreparedA.SourceDatasetDigest);

	TestTrue(TEXT("Commit route A succeeds"), Route->CommitPreparedRoute(Error));
	TestEqual(TEXT("Commit increments generation once"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(1));
	const FRouteArtifactSourceIdentity ActiveA = Route->GetActiveSourceIdentity();
	TestTrue(TEXT("Committed source identity remains available"), ActiveA.IsValid());
	TestTrue(TEXT("Commit preserves prepared identity"), SameSourceIdentity(ActiveA, PreparedA));

	AddExpectedError(
		TEXT("NPZ is too small to contain a ZIP directory"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(TEXT("Failed Prepare rejects corrupt NPZ"),
		Route->PrepareRouteImport(BrokenRoute, Error));
	TestTrue(TEXT("Failed Prepare preserves active source"),
		SameSourceIdentity(Route->GetActiveSourceIdentity(), ActiveA));
	TestEqual(TEXT("Failed Prepare does not change generation"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(1));
	AddExpectedError(
		TEXT("Unable to read NPZ file"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(TEXT("Failed Load rejects missing NPZ"),
		Route->LoadRoute(FPaths::Combine(Directory, TEXT("Missing.npz")), Error));
	TestTrue(TEXT("Failed Load preserves active source"),
		SameSourceIdentity(Route->GetActiveSourceIdentity(), ActiveA));
	TestEqual(TEXT("Failed Load does not change generation"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(1));

	TestTrue(TEXT("Prepare route B succeeds"), Route->PrepareRouteImport(RouteB, Error));
	const FRouteArtifactSourceIdentity PreparedB = Route->GetPreparedSourceIdentity();
	TestTrue(TEXT("Same bytes have same SourceDatasetDigest"),
		PreparedB.SourceDatasetDigest == ActiveA.SourceDatasetDigest);
	TestEqual(TEXT("Different source display basename retained"),
		PreparedB.SourceDisplayFileName, FString(TEXT("Route-B.npz")));
	TestEqual(TEXT("Second Prepare does not change generation"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(1));
	TestTrue(TEXT("Commit route B succeeds"), Route->CommitPreparedRoute(Error));
	TestEqual(TEXT("Second Commit increments once"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(2));
	TestTrue(TEXT("Route B identity remains active"),
		SameSourceIdentity(Route->GetActiveSourceIdentity(), PreparedB));

	Route->UnloadRoute();
	TestFalse(TEXT("Unload clears active source identity"),
		Route->GetActiveSourceIdentity().IsValid());
	TestEqual(TEXT("Unload increments generation once"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(3));
	Route->UnloadRoute();
	TestEqual(TEXT("Second Unload does not increment"),
		Route->GetRouteDatasetGeneration(), static_cast<int64>(3));

	FRouteArtifactDigest256 Digest;
	TestTrue(TEXT("Source bytes hash directly"),
		FRouteArtifactSha256::HashBytes(RouteBytes, Digest, Error));
	FRouteArtifactSourceIdentity DisplayOnly;
	TestTrue(TEXT("Source identity strips absolute path"),
		FRouteArtifactSourceIdentity::Create(
			Digest,
			RouteBytes.Num(),
			TEXT("D:\\Customer\\Shows\\DisplayOnly.npz"),
			DisplayOnly,
			Error));
	TestEqual(TEXT("Only basename is retained"),
		DisplayOnly.SourceDisplayFileName, FString(TEXT("DisplayOnly.npz")));

	GameInstance->Shutdown();
	IFileManager::Get().Delete(*RouteA, false, true, true);
	IFileManager::Get().Delete(*RouteB, false, true, true);
	IFileManager::Get().Delete(*BrokenRoute, false, true, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
