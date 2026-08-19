#include "RouteArtifactManifestV2.h"

#include "RouteArtifactSharedAdapterV2.h"

namespace
{
	using FSharedManifestV2 = dronegcs::route_artifact::v2::Manifest;

	bool ToShared(
		const FRouteArtifactManifestV2& Source,
		FSharedManifestV2& Out,
		FString& OutError)
	{
		Out = {};
		Out.SchemaVersion = Source.ArtifactSchemaVersion;
		Out.ArtifactSetId = RouteArtifactSharedAdapter::ToUtf8(Source.ArtifactSetID);
		Out.CreatedAtUtc = Source.CreatedAtUtc;
		Out.Source.SourceDatasetDigest = RouteArtifactSharedAdapter::ToShared(
			Source.SourceIdentity.SourceDatasetDigest);
		Out.Source.SourceByteSize = Source.SourceIdentity.SourceByteSize;
		Out.Source.SourceDisplayFileName = RouteArtifactSharedAdapter::ToUtf8(
			Source.SourceIdentity.SourceDisplayFileName);
		Out.FrameCount = Source.FrameCount;
		Out.RouteSlotCount = Source.RouteSlotCount;
		Out.HasRange = Source.bHasRange;
		Out.RangeStart = Source.RangeStart;
		Out.RangeEnd = Source.RangeEnd;
		Out.ShowOriginLatitudeE7 = Source.ShowOriginLatitudeE7;
		Out.ShowOriginLongitudeE7 = Source.ShowOriginLongitudeE7;
		Out.WaypointGeneratorVersion = RouteArtifactSharedAdapter::ToUtf8(
			Source.WaypointGeneratorVersion);
		Out.GeneratorInputDigest = RouteArtifactSharedAdapter::ToShared(
			Source.GeneratorInputDigest);
		Out.Routes.reserve(Source.Routes.Num());
		for (const FRouteArtifactRecord& Route : Source.Routes)
		{
			auto& Shared = Out.Routes.emplace_back();
			Shared.RouteSlotIndex = Route.RouteSlotIndex;
			Shared.RelativePath = RouteArtifactSharedAdapter::ToUtf8(Route.RelativePath);
			Shared.FileByteSize = Route.FileByteSize;
			Shared.RouteArtifactDigest = RouteArtifactSharedAdapter::ToShared(
				Route.RouteArtifactDigest);
		}
		Out.ArtifactSetDigest = RouteArtifactSharedAdapter::ToShared(
			Source.ArtifactSetDigest);
		OutError.Reset();
		return true;
	}

	bool FromShared(
		const FSharedManifestV2& Source,
		FRouteArtifactManifestV2& Out,
		FString& OutError)
	{
		Out = {};
		Out.ArtifactSchemaVersion = Source.SchemaVersion;
		Out.ArtifactSetID = RouteArtifactSharedAdapter::FromUtf8(Source.ArtifactSetId);
		Out.CreatedAtUtc = Source.CreatedAtUtc;
		if (!RouteArtifactSharedAdapter::FromShared(
			Source.Source.SourceDatasetDigest,
			Out.SourceIdentity.SourceDatasetDigest,
			OutError))
		{
			return false;
		}
		Out.SourceIdentity.SourceByteSize = Source.Source.SourceByteSize;
		Out.SourceIdentity.SourceDisplayFileName = RouteArtifactSharedAdapter::FromUtf8(
			Source.Source.SourceDisplayFileName);
		Out.FrameCount = Source.FrameCount;
		Out.RouteSlotCount = Source.RouteSlotCount;
		Out.bHasRange = Source.HasRange;
		Out.RangeStart = Source.RangeStart;
		Out.RangeEnd = Source.RangeEnd;
		Out.ShowOriginLatitudeE7 = Source.ShowOriginLatitudeE7;
		Out.ShowOriginLongitudeE7 = Source.ShowOriginLongitudeE7;
		Out.WaypointGeneratorVersion = RouteArtifactSharedAdapter::FromUtf8(
			Source.WaypointGeneratorVersion);
		if (!RouteArtifactSharedAdapter::FromShared(
			Source.GeneratorInputDigest, Out.GeneratorInputDigest, OutError))
		{
			return false;
		}
		Out.Routes.Reserve(static_cast<int32>(Source.Routes.size()));
		for (const auto& Route : Source.Routes)
		{
			FRouteArtifactRecord& Unreal = Out.Routes.AddDefaulted_GetRef();
			Unreal.RouteSlotIndex = Route.RouteSlotIndex;
			Unreal.RelativePath = RouteArtifactSharedAdapter::FromUtf8(Route.RelativePath);
			Unreal.FileByteSize = Route.FileByteSize;
			if (!RouteArtifactSharedAdapter::FromShared(
				Route.RouteArtifactDigest, Unreal.RouteArtifactDigest, OutError))
			{
				return false;
			}
		}
		return RouteArtifactSharedAdapter::FromShared(
			Source.ArtifactSetDigest, Out.ArtifactSetDigest, OutError);
	}
}

bool RouteArtifactManifestV2::BuildIdentityFromManifest(
	const FRouteArtifactManifestV2& Manifest,
	FRouteArtifactBuildIdentityV2& OutIdentity,
	FString& OutError)
{
	OutIdentity = {};
	OutIdentity.ArtifactSchemaVersion = Manifest.ArtifactSchemaVersion;
	OutIdentity.SourceDatasetDigest = Manifest.SourceIdentity.SourceDatasetDigest;
	OutIdentity.WaypointGeneratorVersion = Manifest.WaypointGeneratorVersion;
	OutIdentity.GeneratorInputDigest = Manifest.GeneratorInputDigest;
	OutIdentity.ShowOriginLatitudeE7 = Manifest.ShowOriginLatitudeE7;
	OutIdentity.ShowOriginLongitudeE7 = Manifest.ShowOriginLongitudeE7;
	OutIdentity.FrameCount = Manifest.FrameCount;
	OutIdentity.RouteSlotCount = Manifest.RouteSlotCount;
	OutIdentity.bHasRange = Manifest.bHasRange;
	OutIdentity.RangeStart = Manifest.RangeStart;
	OutIdentity.RangeEnd = Manifest.RangeEnd;
	return OutIdentity.Validate(OutError);
}

bool RouteArtifactManifestV2::ComputeArtifactSetDigest(
	const FRouteArtifactManifestV2& Manifest,
	FRouteArtifactDigest256& OutDigest,
	FString& OutError)
{
	OutDigest.Reset();
	FSharedManifestV2 Shared;
	if (!ToShared(Manifest, Shared, OutError)) return false;
	dronegcs::route_artifact::Digest256 SharedDigest;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::v2::ComputeArtifactSetDigest(
		Shared, SharedDigest, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	return RouteArtifactSharedAdapter::FromShared(SharedDigest, OutDigest, OutError);
}

bool RouteArtifactManifestV2::SerializeCanonicalManifest(
	const FRouteArtifactManifestV2& Manifest,
	TArray<uint8>& OutUtf8Bytes,
	FString& OutError)
{
	OutUtf8Bytes.Reset();
	FSharedManifestV2 Shared;
	std::string Bytes;
	dronegcs::route_artifact::Error SharedError;
	if (!ToShared(Manifest, Shared, OutError)
		|| !dronegcs::route_artifact::v2::SerializeCanonicalManifest(
			Shared, Bytes, SharedError))
	{
		if (OutError.IsEmpty())
		{
			OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		}
		return false;
	}
	OutUtf8Bytes.Append(
		reinterpret_cast<const uint8*>(Bytes.data()),
		static_cast<int32>(Bytes.size()));
	return true;
}

bool RouteArtifactManifestV2::ParseCanonicalManifest(
	const TArray<uint8>& Utf8Bytes,
	FRouteArtifactManifestV2& OutManifest,
	FString& OutError)
{
	OutManifest = {};
	const std::string Bytes(
		reinterpret_cast<const char*>(Utf8Bytes.GetData()),
		static_cast<size_t>(Utf8Bytes.Num()));
	FSharedManifestV2 Shared;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::v2::ParseCanonicalManifest(
		Bytes, Shared, SharedError)
		|| !FromShared(Shared, OutManifest, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		}
		OutManifest = {};
		return false;
	}
	return true;
}

bool RouteArtifactManifestV2::ValidateManifestStructure(
	const FRouteArtifactManifestV2& Manifest,
	FString& OutError)
{
	FSharedManifestV2 Shared;
	dronegcs::route_artifact::Error SharedError;
	const bool bValid = ToShared(Manifest, Shared, OutError)
		&& dronegcs::route_artifact::v2::ValidateManifest(Shared, SharedError);
	if (!bValid && OutError.IsEmpty())
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
	}
	return bValid;
}

bool RouteArtifactManifestV2::ValidateManifestAgainstExpectedInputs(
	const FRouteArtifactManifestV2& Manifest,
	const FRouteArtifactExpectedInputsV2& Expected,
	FString& OutError)
{
	if (!ValidateManifestStructure(Manifest, OutError)
		|| !Expected.BuildIdentity.Validate(OutError)
		|| !Expected.SourceIdentity.IsValid())
	{
		return false;
	}
	FRouteArtifactBuildIdentityV2 Actual;
	if (!BuildIdentityFromManifest(Manifest, Actual, OutError)
		|| Actual.ArtifactSchemaVersion != Expected.BuildIdentity.ArtifactSchemaVersion
		|| Actual.SourceDatasetDigest != Expected.BuildIdentity.SourceDatasetDigest
		|| Actual.WaypointGeneratorVersion != Expected.BuildIdentity.WaypointGeneratorVersion
		|| Actual.GeneratorInputDigest != Expected.BuildIdentity.GeneratorInputDigest
		|| Actual.ShowOriginLatitudeE7 != Expected.BuildIdentity.ShowOriginLatitudeE7
		|| Actual.ShowOriginLongitudeE7 != Expected.BuildIdentity.ShowOriginLongitudeE7
		|| Actual.FrameCount != Expected.BuildIdentity.FrameCount
		|| Actual.RouteSlotCount != Expected.BuildIdentity.RouteSlotCount
		|| Actual.bHasRange != Expected.BuildIdentity.bHasRange
		|| Actual.RangeStart != Expected.BuildIdentity.RangeStart
		|| Actual.RangeEnd != Expected.BuildIdentity.RangeEnd
		|| Manifest.SourceIdentity.SourceDatasetDigest
			!= Expected.SourceIdentity.SourceDatasetDigest
		|| Manifest.SourceIdentity.SourceByteSize
			!= Expected.SourceIdentity.SourceByteSize
		|| Manifest.SourceIdentity.SourceDisplayFileName
			!= Expected.SourceIdentity.SourceDisplayFileName)
	{
		OutError = TEXT("Route Artifact v2 Manifest does not match captured route inputs.");
		return false;
	}
	return true;
}
