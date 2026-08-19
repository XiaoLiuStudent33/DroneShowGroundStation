#include "RouteArtifactTypesV2.h"

#include "RouteArtifactSharedAdapterV2.h"

bool FRouteArtifactBuildIdentityV2::Validate(FString& OutError) const
{
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::v2::ValidateBuildIdentity(
		RouteArtifactSharedAdapterV2::ToShared(*this), SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutError.Reset();
	return true;
}

bool RouteArtifactIdentityV2::SerializeCanonicalBuildIdentity(
	const FRouteArtifactBuildIdentityV2& Identity,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	OutBytes.Reset();
	std::vector<uint8> SharedBytes;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::v2::SerializeCanonicalBuildIdentity(
		RouteArtifactSharedAdapterV2::ToShared(Identity), SharedBytes, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutBytes.Append(SharedBytes.data(), static_cast<int32>(SharedBytes.size()));
	OutError.Reset();
	return true;
}

bool RouteArtifactIdentityV2::ComputeArtifactSetID(
	const FRouteArtifactBuildIdentityV2& Identity,
	FString& OutArtifactSetID,
	FString& OutError)
{
	OutArtifactSetID.Reset();
	std::string SharedSetID;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::v2::ComputeArtifactSetId(
		RouteArtifactSharedAdapterV2::ToShared(Identity), SharedSetID, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutArtifactSetID = RouteArtifactSharedAdapter::FromUtf8(SharedSetID);
	OutError.Reset();
	return true;
}
