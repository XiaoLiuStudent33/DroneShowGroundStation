#include "RouteArtifactManifest.h"

#include "RouteArtifactSharedAdapter.h"
#include "RouteArtifactWorkspacePaths.h"

namespace
{
	using FSharedManifest = dronegcs::route_artifact::Manifest;

	bool ToShared(const FRouteArtifactManifest& Source, FSharedManifest& Out, FString& OutError)
	{
		Out = {};
		Out.SchemaVersion = Source.ArtifactSchemaVersion;
		Out.ArtifactSetId = RouteArtifactSharedAdapter::ToUtf8(Source.ArtifactSetID);
		Out.CreatedAtUtc = Source.CreatedAtUtc;
		Out.Assignment = RouteArtifactSharedAdapter::ToShared(Source.AssignmentBinding);
		Out.FleetRevisionAtCapture = Source.FleetRevisionAtCapture;
		Out.RouteDatasetGeneration = Source.RouteDatasetGeneration;
		switch (Source.CompletionMode)
		{
		case ERouteAssignmentCompletionMode::Full: Out.Completion = dronegcs::route_artifact::CompletionMode::Full; break;
		case ERouteAssignmentCompletionMode::Degraded: Out.Completion = dronegcs::route_artifact::CompletionMode::Degraded; break;
		default: OutError = TEXT("Manifest CompletionMode is unset or unsupported."); return false;
		}
		switch (Source.FreezePolicy)
		{
		case ERouteAssignmentFreezePolicy::RequireFull: Out.Freeze = dronegcs::route_artifact::FreezePolicy::RequireFull; break;
		case ERouteAssignmentFreezePolicy::AllowDegraded: Out.Freeze = dronegcs::route_artifact::FreezePolicy::AllowDegraded; break;
		default: OutError = TEXT("Manifest FreezePolicy is unset or unsupported."); return false;
		}
		Out.MatchedCount = Source.MatchedCount;
		Out.SpareCount = Source.SpareCount;
		Out.ExcludedCount = Source.ExcludedCount;
		Out.VacantCount = Source.VacantCount;
		Out.Source.SourceDatasetDigest = RouteArtifactSharedAdapter::ToShared(Source.SourceIdentity.SourceDatasetDigest);
		Out.Source.SourceByteSize = Source.SourceIdentity.SourceByteSize;
		Out.Source.SourceDisplayFileName = RouteArtifactSharedAdapter::ToUtf8(Source.SourceIdentity.SourceDisplayFileName);
		Out.FrameCount = Source.FrameCount;
		Out.RouteSlotCount = Source.RouteSlotCount;
		Out.HasRange = Source.bHasRange;
		Out.RangeStart = Source.RangeStart;
		Out.RangeEnd = Source.RangeEnd;
		Out.WaypointGeneratorVersion = RouteArtifactSharedAdapter::ToUtf8(Source.WaypointGeneratorVersion);
		Out.GeneratorInputDigest = RouteArtifactSharedAdapter::ToShared(Source.GeneratorInputDigest);
		Out.Mapping.reserve(Source.MatchedMapping.Num());
		for (const FRouteArtifactMatchedMapping& Mapping : Source.MatchedMapping)
		{
			auto& Shared = Out.Mapping.emplace_back();
			Shared.DroneId = RouteArtifactSharedAdapter::ToUtf8(Mapping.DroneID);
			Shared.RouteSlotIndex = Mapping.RouteSlotIndex;
		}
		Out.Routes.reserve(Source.Routes.Num());
		for (const FRouteArtifactRecord& Route : Source.Routes)
		{
			auto& Shared = Out.Routes.emplace_back();
			Shared.RouteSlotIndex = Route.RouteSlotIndex;
			Shared.RelativePath = RouteArtifactSharedAdapter::ToUtf8(Route.RelativePath);
			Shared.FileByteSize = Route.FileByteSize;
			Shared.RouteArtifactDigest = RouteArtifactSharedAdapter::ToShared(Route.RouteArtifactDigest);
		}
		Out.ArtifactSetDigest = RouteArtifactSharedAdapter::ToShared(Source.ArtifactSetDigest);
		return true;
	}

	bool FromShared(const FSharedManifest& Source, FRouteArtifactManifest& Out, FString& OutError)
	{
		Out = {};
		Out.ArtifactSchemaVersion = Source.SchemaVersion;
		Out.ArtifactSetID = RouteArtifactSharedAdapter::FromUtf8(Source.ArtifactSetId);
		Out.CreatedAtUtc = Source.CreatedAtUtc;
		Out.AssignmentBinding.FleetSessionID = RouteArtifactSharedAdapter::FromUtf8(Source.Assignment.FleetSessionId);
		Out.AssignmentBinding.AssignmentAuthoritySessionID = RouteArtifactSharedAdapter::FromUtf8(Source.Assignment.AssignmentAuthoritySessionId);
		Out.AssignmentBinding.AssignmentRevision = Source.Assignment.AssignmentRevision;
		if (!RouteArtifactSharedAdapter::FromShared(Source.Assignment.AssignmentDigest, Out.AssignmentBinding.AssignmentDigest, OutError)) return false;
		Out.FleetRevisionAtCapture = Source.FleetRevisionAtCapture;
		Out.RouteDatasetGeneration = Source.RouteDatasetGeneration;
		Out.CompletionMode = Source.Completion == dronegcs::route_artifact::CompletionMode::Full
			? ERouteAssignmentCompletionMode::Full : ERouteAssignmentCompletionMode::Degraded;
		Out.FreezePolicy = Source.Freeze == dronegcs::route_artifact::FreezePolicy::RequireFull
			? ERouteAssignmentFreezePolicy::RequireFull : ERouteAssignmentFreezePolicy::AllowDegraded;
		Out.MatchedCount = Source.MatchedCount;
		Out.SpareCount = Source.SpareCount;
		Out.ExcludedCount = Source.ExcludedCount;
		Out.VacantCount = Source.VacantCount;
		if (!RouteArtifactSharedAdapter::FromShared(Source.Source.SourceDatasetDigest, Out.SourceIdentity.SourceDatasetDigest, OutError)) return false;
		Out.SourceIdentity.SourceByteSize = Source.Source.SourceByteSize;
		Out.SourceIdentity.SourceDisplayFileName = RouteArtifactSharedAdapter::FromUtf8(Source.Source.SourceDisplayFileName);
		Out.FrameCount = Source.FrameCount;
		Out.RouteSlotCount = Source.RouteSlotCount;
		Out.bHasRange = Source.HasRange;
		Out.RangeStart = Source.RangeStart;
		Out.RangeEnd = Source.RangeEnd;
		Out.WaypointGeneratorVersion = RouteArtifactSharedAdapter::FromUtf8(Source.WaypointGeneratorVersion);
		if (!RouteArtifactSharedAdapter::FromShared(Source.GeneratorInputDigest, Out.GeneratorInputDigest, OutError)) return false;
		Out.MatchedMapping.Reserve(static_cast<int32>(Source.Mapping.size()));
		for (const auto& Mapping : Source.Mapping)
		{
			auto& Unreal = Out.MatchedMapping.AddDefaulted_GetRef();
			Unreal.DroneID = RouteArtifactSharedAdapter::FromUtf8(Mapping.DroneId);
			Unreal.RouteSlotIndex = Mapping.RouteSlotIndex;
		}
		Out.Routes.Reserve(static_cast<int32>(Source.Routes.size()));
		for (const auto& Route : Source.Routes)
		{
			auto& Unreal = Out.Routes.AddDefaulted_GetRef();
			Unreal.RouteSlotIndex = Route.RouteSlotIndex;
			Unreal.RelativePath = RouteArtifactSharedAdapter::FromUtf8(Route.RelativePath);
			Unreal.FileByteSize = Route.FileByteSize;
			if (!RouteArtifactSharedAdapter::FromShared(Route.RouteArtifactDigest, Unreal.RouteArtifactDigest, OutError)) return false;
		}
		return RouteArtifactSharedAdapter::FromShared(Source.ArtifactSetDigest, Out.ArtifactSetDigest, OutError);
	}

	bool SameBinding(const FRouteArtifactAssignmentBinding& Left, const FRouteArtifactAssignmentBinding& Right)
	{
		return Left.FleetSessionID == Right.FleetSessionID
			&& Left.AssignmentAuthoritySessionID == Right.AssignmentAuthoritySessionID
			&& Left.AssignmentRevision == Right.AssignmentRevision
			&& Left.AssignmentDigest == Right.AssignmentDigest;
	}
}

bool RouteArtifactManifest::BuildIdentityFromManifest(
	const FRouteArtifactManifest& Manifest,
	FRouteArtifactBuildIdentity& OutIdentity,
	FString& OutError)
{
	OutIdentity = {};
	OutIdentity.ArtifactSchemaVersion = Manifest.ArtifactSchemaVersion;
	OutIdentity.AssignmentBinding = Manifest.AssignmentBinding;
	OutIdentity.SourceDatasetDigest = Manifest.SourceIdentity.SourceDatasetDigest;
	OutIdentity.WaypointGeneratorVersion = Manifest.WaypointGeneratorVersion;
	OutIdentity.GeneratorInputDigest = Manifest.GeneratorInputDigest;
	return OutIdentity.Validate(OutError);
}

bool RouteArtifactManifest::ComputeArtifactSetDigest(
	const FRouteArtifactManifest& Manifest,
	FRouteArtifactDigest256& OutDigest,
	FString& OutError)
{
	OutDigest.Reset();
	FSharedManifest Shared;
	if (!ToShared(Manifest, Shared, OutError)) return false;
	dronegcs::route_artifact::Digest256 SharedDigest;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::ComputeArtifactSetDigest(Shared, SharedDigest, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	return RouteArtifactSharedAdapter::FromShared(SharedDigest, OutDigest, OutError);
}

bool RouteArtifactManifest::SerializeCanonicalManifest(
	const FRouteArtifactManifest& Manifest,
	TArray<uint8>& OutUtf8Bytes,
	FString& OutError)
{
	OutUtf8Bytes.Reset();
	FSharedManifest Shared;
	std::string Bytes;
	dronegcs::route_artifact::Error SharedError;
	if (!ToShared(Manifest, Shared, OutError)
		|| !dronegcs::route_artifact::SerializeCanonicalManifest(Shared, Bytes, SharedError))
	{
		if (OutError.IsEmpty()) OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutUtf8Bytes.Append(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
	return true;
}

bool RouteArtifactManifest::ParseCanonicalManifest(
	const TArray<uint8>& Utf8Bytes,
	FRouteArtifactManifest& OutManifest,
	FString& OutError)
{
	OutManifest = {};
	const std::string Bytes(reinterpret_cast<const char*>(Utf8Bytes.GetData()), static_cast<size_t>(Utf8Bytes.Num()));
	FSharedManifest Shared;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::ParseCanonicalManifest(Bytes, Shared, SharedError)
		|| !FromShared(Shared, OutManifest, OutError))
	{
		if (OutError.IsEmpty()) OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		OutManifest = {};
		return false;
	}
	return true;
}

bool RouteArtifactManifest::ValidateManifestStructure(const FRouteArtifactManifest& Manifest, FString& OutError)
{
	FSharedManifest Shared;
	dronegcs::route_artifact::Error SharedError;
	const bool bValid = ToShared(Manifest, Shared, OutError)
		&& dronegcs::route_artifact::ValidateManifest(Shared, SharedError);
	if (!bValid && OutError.IsEmpty()) OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
	return bValid;
}

bool RouteArtifactManifest::ValidateManifestAgainstExpectedInputs(
	const FRouteArtifactManifest& Manifest,
	const FRouteArtifactExpectedInputs& Expected,
	FString& OutError)
{
	if (!ValidateManifestStructure(Manifest, OutError)
		|| !Expected.BuildIdentity.Validate(OutError)
		|| !Expected.SourceIdentity.IsValid()) return false;
	FRouteArtifactBuildIdentity Actual;
	if (!BuildIdentityFromManifest(Manifest, Actual, OutError)
		|| Actual.ArtifactSchemaVersion != Expected.BuildIdentity.ArtifactSchemaVersion
		|| !SameBinding(Actual.AssignmentBinding, Expected.BuildIdentity.AssignmentBinding)
		|| Actual.SourceDatasetDigest != Expected.BuildIdentity.SourceDatasetDigest
		|| Actual.WaypointGeneratorVersion != Expected.BuildIdentity.WaypointGeneratorVersion
		|| Actual.GeneratorInputDigest != Expected.BuildIdentity.GeneratorInputDigest
		|| Manifest.SourceIdentity.SourceDatasetDigest != Expected.SourceIdentity.SourceDatasetDigest
		|| Manifest.SourceIdentity.SourceByteSize != Expected.SourceIdentity.SourceByteSize
		|| Manifest.SourceIdentity.SourceDisplayFileName != Expected.SourceIdentity.SourceDisplayFileName)
	{
		OutError = TEXT("Manifest does not match the expected Assignment/Build/Source identity.");
		return false;
	}
	return true;
}

bool RouteArtifactManifest::ValidateArtifactFiles(
	const FString& ArtifactRoot,
	const FRouteArtifactManifest& Manifest,
	FString& OutError)
{
	if (!ValidateManifestStructure(Manifest, OutError)) return false;
	for (const FRouteArtifactRecord& Route : Manifest.Routes)
	{
		FString ResolvedPath;
		if (!RouteArtifactWorkspacePaths::ResolvePathUnderRoot(ArtifactRoot, Route.RelativePath, ResolvedPath, OutError)) return false;
		FRouteArtifactDigest256 ActualDigest;
		int64 ActualByteSize = 0;
		if (!FRouteArtifactSha256::HashFileStreaming(ResolvedPath, ActualDigest, ActualByteSize, OutError)
			|| ActualByteSize != Route.FileByteSize || ActualDigest != Route.RouteArtifactDigest)
		{
			if (OutError.IsEmpty()) OutError = FString::Printf(
				TEXT("Artifact route file integrity mismatch for RouteSlot %d."), Route.RouteSlotIndex);
			return false;
		}
	}
	return true;
}
