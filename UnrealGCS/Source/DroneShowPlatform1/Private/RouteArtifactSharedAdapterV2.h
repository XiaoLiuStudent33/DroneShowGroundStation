#pragma once

#include "RouteArtifactContractV2.h"
#include "RouteArtifactSharedAdapter.h"
#include "RouteArtifactTypesV2.h"

namespace RouteArtifactSharedAdapterV2
{
	inline dronegcs::route_artifact::v2::BuildIdentity ToShared(
		const FRouteArtifactBuildIdentityV2& Value)
	{
		dronegcs::route_artifact::v2::BuildIdentity Result;
		Result.SchemaVersion = Value.ArtifactSchemaVersion;
		Result.SourceDatasetDigest = RouteArtifactSharedAdapter::ToShared(
			Value.SourceDatasetDigest);
		Result.WaypointGeneratorVersion = RouteArtifactSharedAdapter::ToUtf8(
			Value.WaypointGeneratorVersion);
		Result.GeneratorInputDigest = RouteArtifactSharedAdapter::ToShared(
			Value.GeneratorInputDigest);
		Result.ShowOriginLatitudeE7 = Value.ShowOriginLatitudeE7;
		Result.ShowOriginLongitudeE7 = Value.ShowOriginLongitudeE7;
		Result.FrameCount = Value.FrameCount;
		Result.RouteSlotCount = Value.RouteSlotCount;
		Result.HasRange = Value.bHasRange;
		Result.RangeStart = Value.RangeStart;
		Result.RangeEnd = Value.RangeEnd;
		return Result;
	}
}
