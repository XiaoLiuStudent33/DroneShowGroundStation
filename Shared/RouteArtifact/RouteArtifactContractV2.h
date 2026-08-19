#pragma once

#include "RouteArtifactContract.h"

namespace dronegcs::route_artifact::v2
{
	inline constexpr std::uint16_t ArtifactSchemaVersion = 2;

	/** Stable inputs that determine one pure Route Artifact set. */
	struct BuildIdentity
	{
		std::uint16_t SchemaVersion = ArtifactSchemaVersion;
		Digest256 SourceDatasetDigest;
		std::string WaypointGeneratorVersion;
		Digest256 GeneratorInputDigest;
		std::int32_t ShowOriginLatitudeE7 = 0;
		std::int32_t ShowOriginLongitudeE7 = 0;
		std::int32_t FrameCount = 0;
		std::int32_t RouteSlotCount = 0;
		bool HasRange = false;
		std::int32_t RangeStart = 0;
		std::int32_t RangeEnd = 0;
	};

	/** Pure Route content manifest. Fleet and Assignment authority are absent by contract. */
	struct Manifest
	{
		std::uint16_t SchemaVersion = ArtifactSchemaVersion;
		std::string ArtifactSetId;
		std::int64_t CreatedAtUtc = 0;
		SourceIdentity Source;
		std::int32_t FrameCount = 0;
		std::int32_t RouteSlotCount = 0;
		bool HasRange = false;
		std::int32_t RangeStart = 0;
		std::int32_t RangeEnd = 0;
		std::int32_t ShowOriginLatitudeE7 = 0;
		std::int32_t ShowOriginLongitudeE7 = 0;
		std::string WaypointGeneratorVersion;
		Digest256 GeneratorInputDigest;
		std::vector<RouteRecord> Routes;
		Digest256 ArtifactSetDigest;
	};

	inline bool ValidateRouteDomain(
		const std::int32_t FrameCount,
		const std::int32_t RouteSlotCount,
		const bool HasRange,
		const std::int32_t RangeStart,
		const std::int32_t RangeEnd,
		Error& ErrorValue)
	{
		if (FrameCount <= 0 || RouteSlotCount <= 0)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity,
				"Route Artifact v2 frame and route counts must be positive.");
		if ((HasRange && (RangeStart < 0 || RangeEnd < RangeStart || RangeEnd >= FrameCount))
			|| (!HasRange && (RangeStart != 0 || RangeEnd != 0)))
			return Fail(ErrorValue, ErrorCode::InvalidIdentity,
				"Route Artifact v2 range is invalid.");
		return true;
	}

	inline bool ValidateBuildIdentity(const BuildIdentity& Value, Error& ErrorValue)
	{
		if (Value.SchemaVersion != ArtifactSchemaVersion)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity,
				"Route Artifact build identity is not schema v2.");
		if (!Value.SourceDatasetDigest.IsSet
			|| !IsCanonicalString(Value.WaypointGeneratorVersion)
			|| !Value.GeneratorInputDigest.IsSet)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity,
				"Route Artifact v2 content identity is incomplete.");
		if (Value.ShowOriginLatitudeE7 < -900000000
			|| Value.ShowOriginLatitudeE7 > 900000000
			|| Value.ShowOriginLongitudeE7 < -1800000000
			|| Value.ShowOriginLongitudeE7 > 1800000000)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity,
				"Route Artifact v2 normalized ShowOrigin is outside the E7 domain.");
		if (!ValidateRouteDomain(
			Value.FrameCount, Value.RouteSlotCount, Value.HasRange,
			Value.RangeStart, Value.RangeEnd, ErrorValue))
			return false;
		ErrorValue = {};
		return true;
	}

	inline bool SerializeCanonicalBuildIdentity(
		const BuildIdentity& Identity,
		std::vector<std::uint8_t>& Out,
		Error& ErrorValue)
	{
		Out.clear();
		if (!ValidateBuildIdentity(Identity, ErrorValue)) return false;
		auto String = [&Out](const std::string& Value)
		{
			AppendU32(Out, static_cast<std::uint32_t>(Value.size()));
			Out.insert(Out.end(), Value.begin(), Value.end());
		};
		AppendU16(Out, Identity.SchemaVersion);
		Out.insert(Out.end(), Identity.SourceDatasetDigest.Bytes.begin(),
			Identity.SourceDatasetDigest.Bytes.end());
		String(Identity.WaypointGeneratorVersion);
		Out.insert(Out.end(), Identity.GeneratorInputDigest.Bytes.begin(),
			Identity.GeneratorInputDigest.Bytes.end());
		AppendU32(Out, static_cast<std::uint32_t>(Identity.ShowOriginLatitudeE7));
		AppendU32(Out, static_cast<std::uint32_t>(Identity.ShowOriginLongitudeE7));
		AppendU32(Out, static_cast<std::uint32_t>(Identity.FrameCount));
		AppendU32(Out, static_cast<std::uint32_t>(Identity.RouteSlotCount));
		Out.push_back(Identity.HasRange ? 1U : 0U);
		AppendU32(Out, static_cast<std::uint32_t>(Identity.RangeStart));
		AppendU32(Out, static_cast<std::uint32_t>(Identity.RangeEnd));
		ErrorValue = {};
		return true;
	}

	inline bool ComputeArtifactSetId(
		const BuildIdentity& Identity,
		std::string& Out,
		Error& ErrorValue)
	{
		Out.clear();
		std::vector<std::uint8_t> Bytes;
		Digest256 Digest;
		if (!SerializeCanonicalBuildIdentity(Identity, Bytes, ErrorValue)
			|| !Sha256::Hash(Bytes.data(), Bytes.size(), Digest))
			return Fail(ErrorValue, ErrorCode::InvalidIdentity,
				"Route Artifact v2 ArtifactSetID SHA-256 failed.");
		Out = DigestToLowerHex(Digest);
		return true;
	}

	inline BuildIdentity BuildIdentityFromManifest(const Manifest& Value)
	{
		BuildIdentity Result;
		Result.SchemaVersion = Value.SchemaVersion;
		Result.SourceDatasetDigest = Value.Source.SourceDatasetDigest;
		Result.WaypointGeneratorVersion = Value.WaypointGeneratorVersion;
		Result.GeneratorInputDigest = Value.GeneratorInputDigest;
		Result.ShowOriginLatitudeE7 = Value.ShowOriginLatitudeE7;
		Result.ShowOriginLongitudeE7 = Value.ShowOriginLongitudeE7;
		Result.FrameCount = Value.FrameCount;
		Result.RouteSlotCount = Value.RouteSlotCount;
		Result.HasRange = Value.HasRange;
		Result.RangeStart = Value.RangeStart;
		Result.RangeEnd = Value.RangeEnd;
		return Result;
	}

	inline bool SerializeManifestInternal(
		const Manifest& Value,
		const bool IncludeDigest,
		std::string& Out)
	{
		using detail::AppendJsonString;
		std::vector<RouteRecord> Routes = Value.Routes;
		std::sort(Routes.begin(), Routes.end(), [](const RouteRecord& Left, const RouteRecord& Right)
		{
			return Left.RouteSlotIndex < Right.RouteSlotIndex;
		});
		Out.clear();
		Out.reserve(768 + Routes.size() * 180);
		auto Key = [&Out](const char* Name) { AppendJsonString(Out, Name); Out.push_back(':'); };
		auto StringField = [&Out, &Key](const char* Name, const std::string& Text)
		{
			Key(Name); AppendJsonString(Out, Text);
		};
		auto IntegerField = [&Out, &Key](const char* Name, const std::int64_t Number)
		{
			Key(Name); Out += std::to_string(Number);
		};
		Out.push_back('{');
		IntegerField("artifactSchemaVersion", Value.SchemaVersion); Out.push_back(',');
		StringField("artifactSetId", Value.ArtifactSetId); Out.push_back(',');
		IntegerField("createdAtUtc", Value.CreatedAtUtc); Out.push_back(',');
		Key("source"); Out.push_back('{');
		StringField("sourceDatasetDigest", DigestToLowerHex(Value.Source.SourceDatasetDigest)); Out.push_back(',');
		IntegerField("sourceByteSize", Value.Source.SourceByteSize); Out.push_back(',');
		StringField("sourceDisplayFileName", Value.Source.SourceDisplayFileName); Out.push_back(',');
		IntegerField("frameCount", Value.FrameCount); Out.push_back(',');
		IntegerField("routeSlotCount", Value.RouteSlotCount); Out += "},";
		Key("range"); Out.push_back('{'); Key("hasRange"); Out += Value.HasRange ? "true," : "false,";
		Key("start"); Out += Value.HasRange ? std::to_string(Value.RangeStart) : "null"; Out.push_back(',');
		Key("end"); Out += Value.HasRange ? std::to_string(Value.RangeEnd) : "null"; Out += "},";
		Key("showOrigin"); Out.push_back('{');
		IntegerField("latitudeE7", Value.ShowOriginLatitudeE7); Out.push_back(',');
		IntegerField("longitudeE7", Value.ShowOriginLongitudeE7); Out += "},";
		Key("generator"); Out.push_back('{');
		StringField("waypointGeneratorVersion", Value.WaypointGeneratorVersion); Out.push_back(',');
		StringField("generatorInputDigest", DigestToLowerHex(Value.GeneratorInputDigest)); Out += "},";
		Key("routes"); Out.push_back('[');
		for (std::size_t Index = 0; Index < Routes.size(); ++Index)
		{
			if (Index) Out.push_back(',');
			Out.push_back('{'); IntegerField("routeSlotIndex", Routes[Index].RouteSlotIndex); Out.push_back(',');
			StringField("relativePath", Routes[Index].RelativePath); Out.push_back(',');
			IntegerField("fileByteSize", Routes[Index].FileByteSize); Out.push_back(',');
			StringField("routeArtifactDigest", DigestToLowerHex(Routes[Index].RouteArtifactDigest)); Out.push_back('}');
		}
		Out.push_back(']');
		if (IncludeDigest)
		{
			Out.push_back(',');
			StringField("artifactSetDigest", DigestToLowerHex(Value.ArtifactSetDigest));
		}
		Out.push_back('}');
		return true;
	}

	inline bool ComputeArtifactSetDigest(
		const Manifest& Value,
		Digest256& Out,
		Error& ErrorValue)
	{
		Out = {};
		if (!Value.Source.SourceDatasetDigest.IsSet || !Value.GeneratorInputDigest.IsSet)
			return Fail(ErrorValue, ErrorCode::InvalidDigest,
				"Route Artifact v2 manifest digest inputs are invalid.");
		for (const RouteRecord& Route : Value.Routes)
			if (!Route.RouteArtifactDigest.IsSet)
				return Fail(ErrorValue, ErrorCode::InvalidDigest,
					"Route Artifact v2 route digest is invalid.");
		std::string Bytes;
		SerializeManifestInternal(Value, false, Bytes);
		if (!Sha256::Hash(Bytes.data(), Bytes.size(), Out))
			return Fail(ErrorValue, ErrorCode::DigestMismatch,
				"Route Artifact v2 ArtifactSetDigest SHA-256 failed.");
		ErrorValue = {};
		return true;
	}

	inline bool ValidateManifest(const Manifest& Value, Error& ErrorValue)
	{
		if (Value.SchemaVersion != ArtifactSchemaVersion)
			return Fail(ErrorValue, ErrorCode::InvalidManifest,
				"Unsupported Route Artifact schema version; expected v2.");
		if (!ValidateArtifactSetId(Value.ArtifactSetId, ErrorValue)
			|| Value.CreatedAtUtc <= 0 || Value.CreatedAtUtc > MaxExactJsonInteger)
			return Fail(ErrorValue, ErrorCode::InvalidManifest,
				"Route Artifact v2 manifest identity is invalid.");
		if (!Value.Source.SourceDatasetDigest.IsSet || Value.Source.SourceByteSize < 0
			|| !IsCanonicalString(Value.Source.SourceDisplayFileName)
			|| Value.Source.SourceDisplayFileName == "."
			|| Value.Source.SourceDisplayFileName == ".."
			|| Value.Source.SourceDisplayFileName.find('/') != std::string::npos
			|| Value.Source.SourceDisplayFileName.find('\\') != std::string::npos)
			return Fail(ErrorValue, ErrorCode::InvalidManifest,
				"Route Artifact v2 source identity is invalid.");
		if (!ValidateBuildIdentity(BuildIdentityFromManifest(Value), ErrorValue))
			return false;
		if (Value.Routes.size() != static_cast<std::size_t>(Value.RouteSlotCount))
			return Fail(ErrorValue, ErrorCode::InvalidManifest,
				"Route Artifact v2 does not cover the complete route domain.");
		for (std::int32_t Slot = 0; Slot < Value.RouteSlotCount; ++Slot)
		{
			std::string Expected;
			if (!BuildRouteRelativePath(Slot, Expected, ErrorValue)) return false;
			const RouteRecord& Route = Value.Routes[static_cast<std::size_t>(Slot)];
			if (Route.RouteSlotIndex != Slot || Route.RelativePath != Expected
				|| Route.FileByteSize < 0 || !Route.RouteArtifactDigest.IsSet)
				return Fail(ErrorValue, ErrorCode::InvalidManifest,
					"Route Artifact v2 route domain is invalid or noncanonical.");
		}
		std::string ExpectedSetId;
		if (!ComputeArtifactSetId(BuildIdentityFromManifest(Value), ExpectedSetId, ErrorValue)
			|| ExpectedSetId != Value.ArtifactSetId)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity,
				"Route Artifact v2 ArtifactSetID mismatch.");
		Digest256 ExpectedDigest;
		if (!Value.ArtifactSetDigest.IsSet
			|| !ComputeArtifactSetDigest(Value, ExpectedDigest, ErrorValue)
			|| ExpectedDigest != Value.ArtifactSetDigest)
			return Fail(ErrorValue, ErrorCode::DigestMismatch,
				"Route Artifact v2 ArtifactSetDigest mismatch.");
		ErrorValue = {};
		return true;
	}

	inline bool SerializeCanonicalManifest(
		const Manifest& Value,
		std::string& Out,
		Error& ErrorValue)
	{
		Out.clear();
		if (!ValidateManifest(Value, ErrorValue)) return false;
		return SerializeManifestInternal(Value, true, Out);
	}

	inline bool ParseCanonicalManifest(
		const std::string& Bytes,
		Manifest& Out,
		Error& ErrorValue)
	{
		Out = {};
		if (Bytes.empty() || (Bytes.size() >= 3
			&& static_cast<unsigned char>(Bytes[0]) == 0xef
			&& static_cast<unsigned char>(Bytes[1]) == 0xbb
			&& static_cast<unsigned char>(Bytes[2]) == 0xbf))
			return Fail(ErrorValue, ErrorCode::MalformedJson,
				"Route Artifact v2 manifest must be non-empty UTF-8 without BOM.");
		detail::JsonValue Root;
		if (!detail::JsonParser(Bytes, ErrorValue).Parse(Root)) return false;
		auto String = [&ErrorValue](const detail::JsonValue* Value, std::string& Target)
		{
			if (!Value || Value->Type != detail::JsonValue::Kind::String)
				return Fail(ErrorValue, ErrorCode::WrongFieldType,
					"Route Artifact v2 manifest field must be a string.");
			Target = Value->String;
			return true;
		};
		auto Integer = [&ErrorValue](const detail::JsonValue* Value, std::int64_t& Target)
		{
			if (!Value || Value->Type != detail::JsonValue::Kind::Integer)
				return Fail(ErrorValue, ErrorCode::WrongFieldType,
					"Route Artifact v2 manifest field must be an integer.");
			Target = Value->Integer;
			return true;
		};
		auto Int32 = [&Integer, &ErrorValue](const detail::JsonValue* Value, std::int32_t& Target)
		{
			std::int64_t Number = 0;
			if (!Integer(Value, Number)
				|| Number < std::numeric_limits<std::int32_t>::min()
				|| Number > std::numeric_limits<std::int32_t>::max())
				return Fail(ErrorValue, ErrorCode::IntegerOutOfRange,
					"Route Artifact v2 manifest integer is outside int32 range.");
			Target = static_cast<std::int32_t>(Number);
			return true;
		};

		std::int32_t Schema = 0;
		if (!Int32(detail::Field(Root, "artifactSchemaVersion"), Schema)) return false;
		if (Schema != ArtifactSchemaVersion)
			return Fail(ErrorValue, ErrorCode::InvalidManifest,
				"Unsupported Route Artifact schema version; expected v2.");
		if (!detail::ExactFields(Root, {"artifactSchemaVersion","artifactSetId","createdAtUtc",
			"source","range","showOrigin","generator","routes","artifactSetDigest"}))
			return Fail(ErrorValue, ErrorCode::UnknownOrMissingField,
				"Route Artifact v2 manifest has missing or unknown fields.");
		Out.SchemaVersion = static_cast<std::uint16_t>(Schema);
		if (!String(detail::Field(Root, "artifactSetId"), Out.ArtifactSetId)
			|| !Integer(detail::Field(Root, "createdAtUtc"), Out.CreatedAtUtc)) return false;

		std::string DigestText;
		const detail::JsonValue* Source = detail::Field(Root, "source");
		if (!Source || !detail::ExactFields(*Source,
			{"sourceDatasetDigest","sourceByteSize","sourceDisplayFileName","frameCount","routeSlotCount"}))
			return Fail(ErrorValue, ErrorCode::UnknownOrMissingField,
				"Route Artifact v2 source has missing or unknown fields.");
		if (!String(detail::Field(*Source, "sourceDatasetDigest"), DigestText)
			|| !ParseLowerHexDigest(DigestText, Out.Source.SourceDatasetDigest, ErrorValue)
			|| !Integer(detail::Field(*Source, "sourceByteSize"), Out.Source.SourceByteSize)
			|| !String(detail::Field(*Source, "sourceDisplayFileName"), Out.Source.SourceDisplayFileName)
			|| !Int32(detail::Field(*Source, "frameCount"), Out.FrameCount)
			|| !Int32(detail::Field(*Source, "routeSlotCount"), Out.RouteSlotCount)) return false;

		const detail::JsonValue* Range = detail::Field(Root, "range");
		if (!Range || !detail::ExactFields(*Range, {"hasRange","start","end"}))
			return Fail(ErrorValue, ErrorCode::UnknownOrMissingField,
				"Route Artifact v2 range has missing or unknown fields.");
		const detail::JsonValue* HasRange = detail::Field(*Range, "hasRange");
		if (!HasRange || HasRange->Type != detail::JsonValue::Kind::Bool)
			return Fail(ErrorValue, ErrorCode::WrongFieldType,
				"Route Artifact v2 hasRange must be boolean.");
		Out.HasRange = HasRange->Boolean;
		if (Out.HasRange)
		{
			if (!Int32(detail::Field(*Range, "start"), Out.RangeStart)
				|| !Int32(detail::Field(*Range, "end"), Out.RangeEnd)) return false;
		}
		else
		{
			const auto* Start = detail::Field(*Range, "start");
			const auto* End = detail::Field(*Range, "end");
			if (!Start || !End || Start->Type != detail::JsonValue::Kind::Null
				|| End->Type != detail::JsonValue::Kind::Null)
				return Fail(ErrorValue, ErrorCode::WrongFieldType,
					"Route Artifact v2 absent range must use null bounds.");
		}

		const detail::JsonValue* Origin = detail::Field(Root, "showOrigin");
		if (!Origin || !detail::ExactFields(*Origin, {"latitudeE7","longitudeE7"}))
			return Fail(ErrorValue, ErrorCode::UnknownOrMissingField,
				"Route Artifact v2 ShowOrigin has missing or unknown fields.");
		if (!Int32(detail::Field(*Origin, "latitudeE7"), Out.ShowOriginLatitudeE7)
			|| !Int32(detail::Field(*Origin, "longitudeE7"), Out.ShowOriginLongitudeE7)) return false;

		const detail::JsonValue* Generator = detail::Field(Root, "generator");
		if (!Generator || !detail::ExactFields(*Generator,
			{"waypointGeneratorVersion","generatorInputDigest"}))
			return Fail(ErrorValue, ErrorCode::UnknownOrMissingField,
				"Route Artifact v2 generator has missing or unknown fields.");
		if (!String(detail::Field(*Generator, "waypointGeneratorVersion"), Out.WaypointGeneratorVersion)
			|| !String(detail::Field(*Generator, "generatorInputDigest"), DigestText)
			|| !ParseLowerHexDigest(DigestText, Out.GeneratorInputDigest, ErrorValue)) return false;

		const detail::JsonValue* Routes = detail::Field(Root, "routes");
		if (!Routes || Routes->Type != detail::JsonValue::Kind::Array)
			return Fail(ErrorValue, ErrorCode::WrongFieldType,
				"Route Artifact v2 routes must be an array.");
		Out.Routes.reserve(Routes->Array.size());
		for (const detail::JsonValue& Entry : Routes->Array)
		{
			if (!detail::ExactFields(Entry,
				{"routeSlotIndex","relativePath","fileByteSize","routeArtifactDigest"}))
				return Fail(ErrorValue, ErrorCode::UnknownOrMissingField,
					"Route Artifact v2 route entry has missing or unknown fields.");
			RouteRecord Parsed;
			if (!Int32(detail::Field(Entry, "routeSlotIndex"), Parsed.RouteSlotIndex)
				|| !String(detail::Field(Entry, "relativePath"), Parsed.RelativePath)
				|| !Integer(detail::Field(Entry, "fileByteSize"), Parsed.FileByteSize)
				|| !String(detail::Field(Entry, "routeArtifactDigest"), DigestText)
				|| !ParseLowerHexDigest(DigestText, Parsed.RouteArtifactDigest, ErrorValue)) return false;
			Out.Routes.push_back(std::move(Parsed));
		}
		if (!String(detail::Field(Root, "artifactSetDigest"), DigestText)
			|| !ParseLowerHexDigest(DigestText, Out.ArtifactSetDigest, ErrorValue)
			|| !ValidateManifest(Out, ErrorValue)) return false;
		std::string Canonical;
		SerializeManifestInternal(Out, true, Canonical);
		if (Canonical != Bytes)
			return Fail(ErrorValue, ErrorCode::NonCanonicalManifest,
				"Route Artifact v2 manifest bytes are not canonical.");
		ErrorValue = {};
		return true;
	}
}
