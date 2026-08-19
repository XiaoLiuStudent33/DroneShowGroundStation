#include "RouteArtifactContractV2.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	namespace artifact = dronegcs::route_artifact;
	namespace artifact_v2 = dronegcs::route_artifact::v2;

	void Expect(const bool Condition, const char* const Message)
	{
		if (!Condition) throw std::runtime_error(Message);
	}

	artifact::Digest256 HashText(const std::string& Text)
	{
		artifact::Digest256 Result;
		Expect(artifact::Sha256::Hash(Text.data(), Text.size(), Result),
			"SHA-256 fixture failed.");
		return Result;
	}

	artifact::Digest256 GeneratorInputDigest(
		const std::int32_t LatitudeE7,
		const std::int32_t LongitudeE7,
		const double ShowOriginAltitude)
	{
		(void)ShowOriginAltitude; // Altitude is deliberately outside generator-input v1.
		static constexpr char Domain[] = "DRONEGCS_WAYPOINT_GENERATOR_INPUT";
		std::vector<std::uint8_t> Bytes(
			reinterpret_cast<const std::uint8_t*>(Domain),
			reinterpret_cast<const std::uint8_t*>(Domain) + sizeof(Domain) - 1);
		artifact::AppendU16(Bytes, 1);
		artifact::AppendU32(Bytes, static_cast<std::uint32_t>(LatitudeE7));
		artifact::AppendU32(Bytes, static_cast<std::uint32_t>(LongitudeE7));
		artifact::Digest256 Result;
		Expect(artifact::Sha256::Hash(Bytes.data(), Bytes.size(), Result),
			"GeneratorInputDigest fixture failed.");
		return Result;
	}

	artifact_v2::BuildIdentity MakeIdentity(
		const std::int32_t LatitudeE7 = 319324710,
		const std::int32_t LongitudeE7 = 1186385310,
		const double Altitude = 0.0)
	{
		artifact_v2::BuildIdentity Identity;
		Identity.SourceDatasetDigest = HashText("route-source-a");
		Identity.WaypointGeneratorVersion = "qgc-wpl110-wgs84-relative-rgb-v1";
		Identity.GeneratorInputDigest = GeneratorInputDigest(
			LatitudeE7, LongitudeE7, Altitude);
		Identity.ShowOriginLatitudeE7 = LatitudeE7;
		Identity.ShowOriginLongitudeE7 = LongitudeE7;
		Identity.FrameCount = 3;
		Identity.RouteSlotCount = 2;
		return Identity;
	}

	std::string SetId(const artifact_v2::BuildIdentity& Identity)
	{
		artifact::Error Error;
		std::string Result;
		Expect(artifact_v2::ComputeArtifactSetId(Identity, Result, Error),
			Error.Detail.c_str());
		return Result;
	}

	artifact_v2::Manifest MakeManifest()
	{
		const artifact_v2::BuildIdentity Identity = MakeIdentity();
		artifact_v2::Manifest Manifest;
		Manifest.ArtifactSetId = SetId(Identity);
		Manifest.CreatedAtUtc = 1776556800123LL;
		Manifest.Source.SourceDatasetDigest = Identity.SourceDatasetDigest;
		Manifest.Source.SourceByteSize = 123456;
		Manifest.Source.SourceDisplayFileName = "show.npz";
		Manifest.FrameCount = Identity.FrameCount;
		Manifest.RouteSlotCount = Identity.RouteSlotCount;
		Manifest.ShowOriginLatitudeE7 = Identity.ShowOriginLatitudeE7;
		Manifest.ShowOriginLongitudeE7 = Identity.ShowOriginLongitudeE7;
		Manifest.WaypointGeneratorVersion = Identity.WaypointGeneratorVersion;
		Manifest.GeneratorInputDigest = Identity.GeneratorInputDigest;
		for (std::int32_t Slot = 0; Slot < Manifest.RouteSlotCount; ++Slot)
		{
			artifact::RouteRecord Route;
			Route.RouteSlotIndex = Slot;
			artifact::Error PathError;
			Expect(artifact::BuildRouteRelativePath(Slot, Route.RelativePath, PathError),
				PathError.Detail.c_str());
			Route.FileByteSize = 1000 + Slot;
			Route.RouteArtifactDigest = HashText("route-" + std::to_string(Slot));
			Manifest.Routes.push_back(std::move(Route));
		}
		artifact::Error Error;
		Expect(artifact_v2::ComputeArtifactSetDigest(
			Manifest, Manifest.ArtifactSetDigest, Error), Error.Detail.c_str());
		return Manifest;
	}

	artifact::Manifest MakeLegacyV1Manifest()
	{
		artifact::Manifest Manifest;
		Manifest.CreatedAtUtc = 1776556800123LL;
		Manifest.Assignment.FleetSessionId = "fleet-v1";
		Manifest.Assignment.AssignmentAuthoritySessionId = "assignment-v1";
		Manifest.Assignment.AssignmentRevision = 7;
		Manifest.Assignment.AssignmentDigest = HashText("assignment-v1");
		Manifest.FleetRevisionAtCapture = 9;
		Manifest.RouteDatasetGeneration = 3;
		Manifest.MatchedCount = 2;
		Manifest.Source.SourceDatasetDigest = HashText("route-source-v1");
		Manifest.Source.SourceByteSize = 456;
		Manifest.Source.SourceDisplayFileName = "legacy.npz";
		Manifest.FrameCount = 2;
		Manifest.RouteSlotCount = 2;
		Manifest.WaypointGeneratorVersion = "qgc-wpl110-wgs84-relative-rgb-v1";
		Manifest.GeneratorInputDigest = GeneratorInputDigest(319324710, 1186385310, 0.0);
		for (std::int32_t Slot = 0; Slot < 2; ++Slot)
		{
			Manifest.Mapping.push_back({ "drone-" + std::to_string(Slot), Slot });
			artifact::RouteRecord Route;
			Route.RouteSlotIndex = Slot;
			artifact::Error PathError;
			Expect(artifact::BuildRouteRelativePath(Slot, Route.RelativePath, PathError),
				PathError.Detail.c_str());
			Route.FileByteSize = 50 + Slot;
			Route.RouteArtifactDigest = HashText("legacy-route-" + std::to_string(Slot));
			Manifest.Routes.push_back(std::move(Route));
		}
		artifact::Error Error;
		Expect(artifact::ComputeArtifactSetId(
			artifact::BuildIdentityFromManifest(Manifest), Manifest.ArtifactSetId, Error),
			Error.Detail.c_str());
		Expect(artifact::ComputeArtifactSetDigest(
			Manifest, Manifest.ArtifactSetDigest, Error), Error.Detail.c_str());
		return Manifest;
	}
}

int RunRouteArtifactV2ContractTests()
{
	try
	{
		artifact::Error Error;
		const artifact_v2::BuildIdentity Baseline = MakeIdentity();
		const std::string BaselineId = SetId(Baseline);

		// A: Fleet/Assignment values exist only outside v2 and cannot affect its ID.
		artifact::AssignmentBinding FirstLegacyBinding{
			"fleet-a", "authority-a", 1, HashText("assignment-a") };
		artifact::AssignmentBinding SecondLegacyBinding{
			"fleet-b", "authority-b", 99, HashText("assignment-b") };
		Expect(FirstLegacyBinding.FleetSessionId != SecondLegacyBinding.FleetSessionId,
			"Legacy assignment fixtures must differ.");
		Expect(SetId(Baseline) == BaselineId,
			"External Fleet/Assignment values changed the v2 ArtifactSetID.");

		// B: source content is part of v2 identity.
		auto Changed = Baseline;
		Changed.SourceDatasetDigest = HashText("route-source-b");
		Expect(SetId(Changed) != BaselineId,
			"SourceDatasetDigest did not change the v2 ArtifactSetID.");

		// C: normalized Lat/Lon and their generator-input digest affect identity.
		const auto ChangedOrigin = MakeIdentity(319324711, 1186385310, 0.0);
		Expect(ChangedOrigin.GeneratorInputDigest != Baseline.GeneratorInputDigest,
			"ShowOrigin Lat/Lon did not change GeneratorInputDigest.");
		Expect(SetId(ChangedOrigin) != BaselineId,
			"ShowOrigin Lat/Lon did not change the v2 ArtifactSetID.");

		// D: ShowOrigin altitude is absent from both contracts.
		const auto ChangedAltitude = MakeIdentity(319324710, 1186385310, 123.456);
		Expect(ChangedAltitude.GeneratorInputDigest == Baseline.GeneratorInputDigest,
			"ShowOrigin altitude changed GeneratorInputDigest.");
		Expect(SetId(ChangedAltitude) == BaselineId,
			"ShowOrigin altitude changed the v2 ArtifactSetID.");

		// E: generator implementation identity is content-significant.
		Changed = Baseline;
		Changed.WaypointGeneratorVersion += "-changed";
		Expect(SetId(Changed) != BaselineId,
			"GeneratorVersion did not change the v2 ArtifactSetID.");

		// F/G: exact canonical v2 manifest round-trip and forbidden-field absence.
		const artifact_v2::Manifest Manifest = MakeManifest();
		std::string Canonical;
		Expect(artifact_v2::SerializeCanonicalManifest(Manifest, Canonical, Error),
			Error.Detail.c_str());
		artifact_v2::Manifest Parsed;
		Expect(artifact_v2::ParseCanonicalManifest(Canonical, Parsed, Error),
			Error.Detail.c_str());
		std::string RoundTrip;
		Expect(artifact_v2::SerializeCanonicalManifest(Parsed, RoundTrip, Error),
			Error.Detail.c_str());
		Expect(Canonical == RoundTrip, "Route Artifact v2 round-trip is not canonical.");
		for (const char* Forbidden : {
			"assignment", "fleetSessionId", "assignmentRevision", "assignmentDigest",
			"routeDatasetGeneration", "completionMode", "freezePolicy", "matchedCount",
			"vacantCount", "spareCount", "excludedCount", "droneId", "mapping" })
		{
			Expect(Canonical.find(Forbidden) == std::string::npos,
				"Route Artifact v2 manifest contains an Assignment/Fleet field.");
		}

		// H: routes must cover one full, ordered, contiguous domain.
		auto BrokenDomain = Manifest;
		BrokenDomain.Routes[1].RouteSlotIndex = 0;
		Expect(!artifact_v2::ValidateManifest(BrokenDomain, Error),
			"Duplicate RouteSlotIndex was accepted by v2.");
		BrokenDomain = Manifest;
		BrokenDomain.Routes.pop_back();
		Expect(!artifact_v2::ValidateManifest(BrokenDomain, Error),
			"Incomplete RouteSlot domain was accepted by v2.");

		// I: digest is deterministic; CreatedAt stays digest-bound but ID-excluded.
		artifact::Digest256 FirstDigest;
		artifact::Digest256 SecondDigest;
		Expect(artifact_v2::ComputeArtifactSetDigest(Manifest, FirstDigest, Error)
			&& artifact_v2::ComputeArtifactSetDigest(Manifest, SecondDigest, Error),
			"Route Artifact v2 digest computation failed.");
		Expect(FirstDigest == SecondDigest,
			"Route Artifact v2 ArtifactSetDigest is not deterministic.");
		auto LaterManifest = Manifest;
		++LaterManifest.CreatedAtUtc;
		artifact::Digest256 LaterDigest;
		Expect(artifact_v2::ComputeArtifactSetDigest(LaterManifest, LaterDigest, Error),
			Error.Detail.c_str());
		Expect(LaterDigest != FirstDigest,
			"CreatedAtUtc is not protected by ArtifactSetDigest.");
		Expect(artifact_v2::BuildIdentityFromManifest(LaterManifest).SourceDatasetDigest
			== artifact_v2::BuildIdentityFromManifest(Manifest).SourceDatasetDigest
			&& SetId(artifact_v2::BuildIdentityFromManifest(LaterManifest)) == BaselineId,
			"CreatedAtUtc changed ArtifactSetID.");

		// J: a canonical v1 manifest is rejected explicitly by the v2 parser.
		std::string LegacyCanonical;
		const artifact::Manifest Legacy = MakeLegacyV1Manifest();
		Expect(artifact::SerializeCanonicalManifest(Legacy, LegacyCanonical, Error),
			Error.Detail.c_str());
		Expect(!artifact_v2::ParseCanonicalManifest(LegacyCanonical, Parsed, Error),
			"Canonical Route Artifact v1 was silently parsed as v2.");
		Expect(Error.Code == artifact::ErrorCode::InvalidManifest
			&& Error.Detail.find("expected v2") != std::string::npos,
			"v1 rejection did not report the explicit v2 schema mismatch.");

		std::string Ready;
		Expect(artifact::SerializeReadyMarker(
			Manifest.ArtifactSetId, Manifest.ArtifactSetDigest, Ready, Error),
			Error.Detail.c_str());
		Expect(Ready.rfind("DRONEGCS_ROUTE_ARTIFACT_READY_V1\n", 0) == 0,
			"READY marker format changed during Route Artifact v2-A.");

		std::cout << "Route Artifact v2 contract tests passed\n";
		return 0;
	}
	catch (const std::exception& Exception)
	{
		std::cerr << "Route Artifact v2 contract tests failed: "
			<< Exception.what() << '\n';
		return 1;
	}
}
