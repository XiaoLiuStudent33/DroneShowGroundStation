#pragma once

#include "RouteArtifactContract.h"
#include "RouteArtifactTypes.h"

namespace RouteArtifactSharedAdapter
{
	inline std::string ToUtf8(const FString& Value)
	{
		FTCHARToUTF8 Converted(*Value, Value.Len());
		return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
	}

	inline FString FromUtf8(const std::string& Value)
	{
		FUTF8ToTCHAR Converted(Value.data(), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	inline FString ErrorText(const dronegcs::route_artifact::Error& Error)
	{
		return FromUtf8(Error.Detail);
	}

	inline dronegcs::route_artifact::Digest256 ToShared(const FRouteArtifactDigest256& Value)
	{
		dronegcs::route_artifact::Digest256 Result;
		if (Value.IsValid())
		{
			FMemory::Memcpy(Result.Bytes.data(), Value.GetData(), Result.Bytes.size());
			Result.IsSet = true;
		}
		return Result;
	}

	inline bool FromShared(
		const dronegcs::route_artifact::Digest256& Value,
		FRouteArtifactDigest256& Out,
		FString& OutError)
	{
		Out.Reset();
		if (!Value.IsSet)
		{
			OutError = TEXT("Shared Artifact digest is unset.");
			return false;
		}
		return FRouteArtifactDigest256::FromBytes(
			Value.Bytes.data(), static_cast<int32>(Value.Bytes.size()), Out, OutError);
	}

	inline dronegcs::route_artifact::AssignmentBinding ToShared(
		const FRouteArtifactAssignmentBinding& Value)
	{
		dronegcs::route_artifact::AssignmentBinding Result;
		Result.FleetSessionId = ToUtf8(Value.FleetSessionID);
		Result.AssignmentAuthoritySessionId = ToUtf8(Value.AssignmentAuthoritySessionID);
		Result.AssignmentRevision = Value.AssignmentRevision;
		Result.AssignmentDigest = ToShared(Value.AssignmentDigest);
		return Result;
	}

	inline dronegcs::route_artifact::BuildIdentity ToShared(
		const FRouteArtifactBuildIdentity& Value)
	{
		dronegcs::route_artifact::BuildIdentity Result;
		Result.SchemaVersion = Value.ArtifactSchemaVersion;
		Result.Assignment = ToShared(Value.AssignmentBinding);
		Result.SourceDatasetDigest = ToShared(Value.SourceDatasetDigest);
		Result.WaypointGeneratorVersion = ToUtf8(Value.WaypointGeneratorVersion);
		Result.GeneratorInputDigest = ToShared(Value.GeneratorInputDigest);
		return Result;
	}
}
