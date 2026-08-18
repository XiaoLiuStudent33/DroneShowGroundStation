#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dronegcs::route_artifact
{
	inline constexpr std::uint16_t ArtifactSchemaVersion = 1;
	inline constexpr std::int64_t MaxExactJsonInteger = 9007199254740991LL;
	inline constexpr std::int32_t RouteSlotsPerShard = 1000;
	inline constexpr char ManifestFilename[] = "manifest.json";
	inline constexpr char ReadyFilename[] = "READY";

	enum class CompletionMode : std::uint8_t { Full, Degraded };
	enum class FreezePolicy : std::uint8_t { RequireFull, AllowDegraded };

	enum class ErrorCode : std::uint8_t
	{
		None,
		InvalidArgument,
		InvalidUtf8,
		InvalidDigest,
		InvalidIdentity,
		InvalidPath,
		MalformedReady,
		NonCanonicalReady,
		MalformedJson,
		UnknownOrMissingField,
		WrongFieldType,
		IntegerOutOfRange,
		InvalidManifest,
		NonCanonicalManifest,
		DigestMismatch
	};

	struct Error
	{
		ErrorCode Code = ErrorCode::None;
		std::string Detail;
	};

	inline bool Fail(Error& ErrorValue, const ErrorCode Code, const char* Detail)
	{
		ErrorValue.Code = Code;
		ErrorValue.Detail = Detail ? Detail : "";
		return false;
	}

	struct Digest256
	{
		std::array<std::uint8_t, 32> Bytes{};
		bool IsSet = false;

		bool operator==(const Digest256& Other) const
		{
			return IsSet == Other.IsSet && Bytes == Other.Bytes;
		}
		bool operator!=(const Digest256& Other) const { return !(*this == Other); }
	};

	struct AssignmentBinding
	{
		std::string FleetSessionId;
		std::string AssignmentAuthoritySessionId;
		std::int64_t AssignmentRevision = 0;
		Digest256 AssignmentDigest;
	};

	struct SourceIdentity
	{
		Digest256 SourceDatasetDigest;
		std::int64_t SourceByteSize = 0;
		std::string SourceDisplayFileName;
	};

	struct BuildIdentity
	{
		std::uint16_t SchemaVersion = ArtifactSchemaVersion;
		AssignmentBinding Assignment;
		Digest256 SourceDatasetDigest;
		std::string WaypointGeneratorVersion;
		Digest256 GeneratorInputDigest;
	};

	struct MatchedMapping
	{
		std::string DroneId;
		std::int32_t RouteSlotIndex = -1;
	};

	struct RouteRecord
	{
		std::int32_t RouteSlotIndex = -1;
		std::string RelativePath;
		std::int64_t FileByteSize = 0;
		Digest256 RouteArtifactDigest;
	};

	struct Manifest
	{
		std::uint16_t SchemaVersion = ArtifactSchemaVersion;
		std::string ArtifactSetId;
		std::int64_t CreatedAtUtc = 0;
		AssignmentBinding Assignment;
		std::int64_t FleetRevisionAtCapture = 0;
		std::int64_t RouteDatasetGeneration = 0;
		CompletionMode Completion = CompletionMode::Full;
		FreezePolicy Freeze = FreezePolicy::RequireFull;
		std::int32_t MatchedCount = 0;
		std::int32_t SpareCount = 0;
		std::int32_t ExcludedCount = 0;
		std::int32_t VacantCount = 0;
		SourceIdentity Source;
		std::int32_t FrameCount = 0;
		std::int32_t RouteSlotCount = 0;
		bool HasRange = false;
		std::int32_t RangeStart = 0;
		std::int32_t RangeEnd = 0;
		std::string WaypointGeneratorVersion;
		Digest256 GeneratorInputDigest;
		std::vector<MatchedMapping> Mapping;
		std::vector<RouteRecord> Routes;
		Digest256 ArtifactSetDigest;
	};

	inline bool IsValidUtf8(const std::string& Value)
	{
		const auto* Data = reinterpret_cast<const std::uint8_t*>(Value.data());
		std::size_t Index = 0;
		while (Index < Value.size())
		{
			const std::uint8_t First = Data[Index++];
			if (First <= 0x7f) continue;
			std::uint32_t CodePoint = 0;
			std::size_t Remaining = 0;
			if (First >= 0xc2 && First <= 0xdf)
			{
				CodePoint = First & 0x1f;
				Remaining = 1;
			}
			else if (First >= 0xe0 && First <= 0xef)
			{
				CodePoint = First & 0x0f;
				Remaining = 2;
			}
			else if (First >= 0xf0 && First <= 0xf4)
			{
				CodePoint = First & 0x07;
				Remaining = 3;
			}
			else return false;
			if (Index + Remaining > Value.size()) return false;
			for (std::size_t Offset = 0; Offset < Remaining; ++Offset)
			{
				const std::uint8_t Next = Data[Index++];
				if ((Next & 0xc0) != 0x80) return false;
				CodePoint = (CodePoint << 6) | (Next & 0x3f);
			}
			if ((Remaining == 2 && CodePoint < 0x800)
				|| (Remaining == 3 && CodePoint < 0x10000)
				|| CodePoint > 0x10ffff
				|| (CodePoint >= 0xd800 && CodePoint <= 0xdfff)) return false;
		}
		return true;
	}

	inline bool IsCanonicalString(const std::string& Value)
	{
		return !Value.empty() && Value.find('\0') == std::string::npos && IsValidUtf8(Value);
	}

	inline std::string DigestToLowerHex(const Digest256& Digest)
	{
		if (!Digest.IsSet) return {};
		static constexpr char Digits[] = "0123456789abcdef";
		std::string Result;
		Result.reserve(64);
		for (const std::uint8_t Byte : Digest.Bytes)
		{
			Result.push_back(Digits[(Byte >> 4) & 0x0f]);
			Result.push_back(Digits[Byte & 0x0f]);
		}
		return Result;
	}

	inline bool ParseLowerHexDigest(
		const std::string& Text,
		Digest256& Out,
		Error& ErrorValue)
	{
		Out = {};
		if (Text.size() != 64)
			return Fail(ErrorValue, ErrorCode::InvalidDigest, "Digest must contain exactly 64 lowercase hex characters.");
		auto Nibble = [](const char Character) -> int
		{
			if (Character >= '0' && Character <= '9') return Character - '0';
			if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
			return -1;
		};
		for (std::size_t Index = 0; Index < Out.Bytes.size(); ++Index)
		{
			const int High = Nibble(Text[Index * 2]);
			const int Low = Nibble(Text[Index * 2 + 1]);
			if (High < 0 || Low < 0)
				return Fail(ErrorValue, ErrorCode::InvalidDigest, "Digest must use lowercase hexadecimal.");
			Out.Bytes[Index] = static_cast<std::uint8_t>((High << 4) | Low);
		}
		Out.IsSet = true;
		ErrorValue = {};
		return true;
	}

	class Sha256 final
	{
	public:
		Sha256() { Reset(); }

		void Reset()
		{
			State = { 0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
				0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U };
			TotalBytes = 0;
			Buffered = 0;
			Finalized = false;
		}

		bool Update(const void* DataValue, const std::size_t Size)
		{
			if (Finalized || (Size > 0 && DataValue == nullptr)) return false;
			const auto* Data = static_cast<const std::uint8_t*>(DataValue);
			TotalBytes += static_cast<std::uint64_t>(Size);
			std::size_t Offset = 0;
			if (Buffered > 0)
			{
				const std::size_t Copy = std::min(Size, Block.size() - Buffered);
				std::memcpy(Block.data() + Buffered, Data, Copy);
				Buffered += Copy;
				Offset += Copy;
				if (Buffered == Block.size())
				{
					Transform(Block.data());
					Buffered = 0;
				}
			}
			while (Offset + Block.size() <= Size)
			{
				Transform(Data + Offset);
				Offset += Block.size();
			}
			if (Offset < Size)
			{
				Buffered = Size - Offset;
				std::memcpy(Block.data(), Data + Offset, Buffered);
			}
			return true;
		}

		bool Finalize(Digest256& Out)
		{
			Out = {};
			if (Finalized) return false;
			const std::uint64_t BitCount = TotalBytes * 8U;
			Block[Buffered++] = 0x80;
			if (Buffered > 56)
			{
				std::fill(Block.begin() + static_cast<std::ptrdiff_t>(Buffered), Block.end(), 0);
				Transform(Block.data());
				Buffered = 0;
			}
			std::fill(Block.begin() + static_cast<std::ptrdiff_t>(Buffered), Block.begin() + 56, 0);
			for (int Shift = 56; Shift >= 0; Shift -= 8)
				Block[56 + (56 - Shift) / 8] = static_cast<std::uint8_t>((BitCount >> Shift) & 0xffU);
			Transform(Block.data());
			for (std::size_t Word = 0; Word < State.size(); ++Word)
				for (int Shift = 24; Shift >= 0; Shift -= 8)
					Out.Bytes[Word * 4 + static_cast<std::size_t>((24 - Shift) / 8)] =
						static_cast<std::uint8_t>((State[Word] >> Shift) & 0xffU);
			Out.IsSet = true;
			Finalized = true;
			return true;
		}

		static bool Hash(const void* Data, const std::size_t Size, Digest256& Out)
		{
			Sha256 Builder;
			return Builder.Update(Data, Size) && Builder.Finalize(Out);
		}

	private:
		static std::uint32_t RotateRight(const std::uint32_t Value, const unsigned Bits)
		{
			return (Value >> Bits) | (Value << (32U - Bits));
		}

		void Transform(const std::uint8_t* Data)
		{
			static constexpr std::array<std::uint32_t, 64> K = {
				0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
				0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
				0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
				0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
				0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
				0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
				0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
				0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U };
			std::array<std::uint32_t, 64> W{};
			for (std::size_t Index = 0; Index < 16; ++Index)
				W[Index] = (static_cast<std::uint32_t>(Data[Index * 4]) << 24)
					| (static_cast<std::uint32_t>(Data[Index * 4 + 1]) << 16)
					| (static_cast<std::uint32_t>(Data[Index * 4 + 2]) << 8)
					| static_cast<std::uint32_t>(Data[Index * 4 + 3]);
			for (std::size_t Index = 16; Index < W.size(); ++Index)
			{
				const std::uint32_t S0 = RotateRight(W[Index - 15], 7) ^ RotateRight(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
				const std::uint32_t S1 = RotateRight(W[Index - 2], 17) ^ RotateRight(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
				W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
			}
			std::uint32_t A=State[0],B=State[1],C=State[2],D=State[3],E=State[4],F=State[5],G=State[6],H=State[7];
			for (std::size_t Index = 0; Index < 64; ++Index)
			{
				const std::uint32_t S1 = RotateRight(E,6)^RotateRight(E,11)^RotateRight(E,25);
				const std::uint32_t Ch = (E&F)^((~E)&G);
				const std::uint32_t T1 = H + S1 + Ch + K[Index] + W[Index];
				const std::uint32_t S0 = RotateRight(A,2)^RotateRight(A,13)^RotateRight(A,22);
				const std::uint32_t Maj = (A&B)^(A&C)^(B&C);
				const std::uint32_t T2 = S0 + Maj;
				H=G; G=F; F=E; E=D+T1; D=C; C=B; B=A; A=T1+T2;
			}
			State[0]+=A; State[1]+=B; State[2]+=C; State[3]+=D;
			State[4]+=E; State[5]+=F; State[6]+=G; State[7]+=H;
		}

		std::array<std::uint32_t, 8> State{};
		std::array<std::uint8_t, 64> Block{};
		std::uint64_t TotalBytes = 0;
		std::size_t Buffered = 0;
		bool Finalized = false;
	};

	inline void AppendU16(std::vector<std::uint8_t>& Bytes, const std::uint16_t Value)
	{
		Bytes.push_back(static_cast<std::uint8_t>((Value >> 8) & 0xff));
		Bytes.push_back(static_cast<std::uint8_t>(Value & 0xff));
	}

	inline void AppendU32(std::vector<std::uint8_t>& Bytes, const std::uint32_t Value)
	{
		for (int Shift = 24; Shift >= 0; Shift -= 8)
			Bytes.push_back(static_cast<std::uint8_t>((Value >> Shift) & 0xff));
	}

	inline void AppendU64(std::vector<std::uint8_t>& Bytes, const std::uint64_t Value)
	{
		for (int Shift = 56; Shift >= 0; Shift -= 8)
			Bytes.push_back(static_cast<std::uint8_t>((Value >> Shift) & 0xff));
	}

	inline bool ValidateAssignmentBinding(const AssignmentBinding& Value, Error& ErrorValue)
	{
		if (!IsCanonicalString(Value.FleetSessionId)
			|| !IsCanonicalString(Value.AssignmentAuthoritySessionId)
			|| Value.AssignmentRevision <= 0 || !Value.AssignmentDigest.IsSet)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity, "Assignment binding is invalid.");
		return true;
	}

	inline bool ValidateBuildIdentity(const BuildIdentity& Value, Error& ErrorValue)
	{
		if (Value.SchemaVersion == 0 || !ValidateAssignmentBinding(Value.Assignment, ErrorValue)
			|| !Value.SourceDatasetDigest.IsSet
			|| !IsCanonicalString(Value.WaypointGeneratorVersion)
			|| !Value.GeneratorInputDigest.IsSet)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity, "Artifact build identity is invalid.");
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
		String(Identity.Assignment.FleetSessionId);
		String(Identity.Assignment.AssignmentAuthoritySessionId);
		AppendU64(Out, static_cast<std::uint64_t>(Identity.Assignment.AssignmentRevision));
		Out.insert(Out.end(), Identity.Assignment.AssignmentDigest.Bytes.begin(), Identity.Assignment.AssignmentDigest.Bytes.end());
		Out.insert(Out.end(), Identity.SourceDatasetDigest.Bytes.begin(), Identity.SourceDatasetDigest.Bytes.end());
		String(Identity.WaypointGeneratorVersion);
		Out.insert(Out.end(), Identity.GeneratorInputDigest.Bytes.begin(), Identity.GeneratorInputDigest.Bytes.end());
		ErrorValue = {};
		return true;
	}

	inline bool ComputeArtifactSetId(const BuildIdentity& Identity, std::string& Out, Error& ErrorValue)
	{
		std::vector<std::uint8_t> Bytes;
		Digest256 Digest;
		if (!SerializeCanonicalBuildIdentity(Identity, Bytes, ErrorValue)
			|| !Sha256::Hash(Bytes.data(), Bytes.size(), Digest))
			return Fail(ErrorValue, ErrorCode::InvalidIdentity, "ArtifactSetID SHA-256 failed.");
		Out = DigestToLowerHex(Digest);
		return true;
	}

	inline bool ValidateArtifactSetId(const std::string& Value, Error& ErrorValue)
	{
		if (Value.size() != 64)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity, "ArtifactSetID must contain 64 lowercase hex characters.");
		for (const char Character : Value)
			if (!((Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f')))
				return Fail(ErrorValue, ErrorCode::InvalidIdentity, "ArtifactSetID must use lowercase hexadecimal.");
		return true;
	}

	inline bool IsReservedWindowsSegment(std::string Segment)
	{
		const std::size_t Dot = Segment.find('.');
		if (Dot != std::string::npos) Segment.resize(Dot);
		std::transform(Segment.begin(), Segment.end(), Segment.begin(),
			[](const unsigned char C) { return static_cast<char>(std::toupper(C)); });
		if (Segment == "CON" || Segment == "PRN" || Segment == "AUX" || Segment == "NUL") return true;
		return Segment.size() == 4 && (Segment.rfind("COM", 0) == 0 || Segment.rfind("LPT", 0) == 0)
			&& Segment[3] >= '1' && Segment[3] <= '9';
	}

	inline bool ValidateArtifactRelativePath(const std::string& Path, Error& ErrorValue)
	{
		if (Path.empty() || !IsValidUtf8(Path) || Path.front() == '/' || Path.back() == '/'
			|| Path.find('\\') != std::string::npos || Path.find("//") != std::string::npos
			|| Path.find(':') != std::string::npos || Path.find('\0') != std::string::npos)
			return Fail(ErrorValue, ErrorCode::InvalidPath, "Artifact relative path is not canonical.");
		for (const char Character : Path)
			if (static_cast<unsigned char>(Character) < 0x20 || Character == '<' || Character == '>'
				|| Character == '"' || Character == '|' || Character == '?' || Character == '*')
				return Fail(ErrorValue, ErrorCode::InvalidPath, "Artifact relative path contains an unsafe character.");
		std::size_t Begin = 0;
		while (Begin < Path.size())
		{
			const std::size_t End = Path.find('/', Begin);
			const std::string Segment = Path.substr(Begin, End == std::string::npos ? std::string::npos : End - Begin);
			if (Segment.empty() || Segment == "." || Segment == ".." || Segment.back() == '.'
				|| Segment.back() == ' ' || IsReservedWindowsSegment(Segment))
				return Fail(ErrorValue, ErrorCode::InvalidPath, "Artifact relative path contains an unsafe segment.");
			if (End == std::string::npos) break;
			Begin = End + 1;
		}
		ErrorValue = {};
		return true;
	}

	inline bool BuildRouteRelativePath(const std::int32_t RouteSlotIndex, std::string& Out, Error& ErrorValue)
	{
		Out.clear();
		if (RouteSlotIndex < 0)
			return Fail(ErrorValue, ErrorCode::InvalidPath, "RouteSlotIndex must be non-negative.");
		const std::int64_t ShardStart = (static_cast<std::int64_t>(RouteSlotIndex) / RouteSlotsPerShard) * RouteSlotsPerShard;
		std::string Shard = std::to_string(ShardStart);
		if (Shard.size() < 8) Shard.insert(Shard.begin(), 8 - Shard.size(), '0');
		Out = "routes/" + Shard + "/" + std::to_string(RouteSlotIndex) + ".waypoints";
		return ValidateArtifactRelativePath(Out, ErrorValue);
	}

	namespace detail
	{
		struct JsonValue
		{
			enum class Kind : std::uint8_t { Null, Bool, Integer, String, Object, Array } Type = Kind::Null;
			bool Boolean = false;
			std::int64_t Integer = 0;
			std::string String;
			std::vector<std::pair<std::string, JsonValue>> Object;
			std::vector<JsonValue> Array;
		};

		inline void AppendUtf8CodePoint(std::string& Out, const std::uint32_t CodePoint)
		{
			if (CodePoint <= 0x7f) Out.push_back(static_cast<char>(CodePoint));
			else if (CodePoint <= 0x7ff)
			{
				Out.push_back(static_cast<char>(0xc0 | (CodePoint >> 6)));
				Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3f)));
			}
			else if (CodePoint <= 0xffff)
			{
				Out.push_back(static_cast<char>(0xe0 | (CodePoint >> 12)));
				Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3f)));
				Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3f)));
			}
			else
			{
				Out.push_back(static_cast<char>(0xf0 | (CodePoint >> 18)));
				Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3f)));
				Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3f)));
				Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3f)));
			}
		}

		class JsonParser final
		{
		public:
			JsonParser(const std::string& Bytes, Error& ErrorValue) : Text(Bytes), ErrorRef(ErrorValue) {}

			bool Parse(JsonValue& Out)
			{
				if (!IsValidUtf8(Text)) return Fail(ErrorRef, ErrorCode::InvalidUtf8, "Manifest is not valid UTF-8.");
				if (!ParseValue(Out, 0) || Position != Text.size())
					return Fail(ErrorRef, ErrorCode::MalformedJson, "Manifest JSON is malformed or has trailing content.");
				return true;
			}

		private:
			bool ParseValue(JsonValue& Out, const int Depth)
			{
				if (Depth > 32 || Position >= Text.size()) return false;
				const char Character = Text[Position];
				if (Character == '{') return ParseObject(Out, Depth + 1);
				if (Character == '[') return ParseArray(Out, Depth + 1);
				if (Character == '"') { Out.Type = JsonValue::Kind::String; return ParseString(Out.String); }
				if (Character == 't' && Match("true")) { Out.Type=JsonValue::Kind::Bool; Out.Boolean=true; return true; }
				if (Character == 'f' && Match("false")) { Out.Type=JsonValue::Kind::Bool; Out.Boolean=false; return true; }
				if (Character == 'n' && Match("null")) { Out.Type=JsonValue::Kind::Null; return true; }
				if (Character == '-' || (Character >= '0' && Character <= '9')) return ParseInteger(Out);
				return false;
			}

			bool ParseObject(JsonValue& Out, const int Depth)
			{
				Out.Type = JsonValue::Kind::Object;
				++Position;
				if (Position < Text.size() && Text[Position] == '}') { ++Position; return true; }
				for (;;)
				{
					std::string Key;
					if (!ParseString(Key) || Position >= Text.size() || Text[Position++] != ':') return false;
					for (const auto& Entry : Out.Object) if (Entry.first == Key) return false;
					JsonValue Value;
					if (!ParseValue(Value, Depth)) return false;
					Out.Object.emplace_back(std::move(Key), std::move(Value));
					if (Position >= Text.size()) return false;
					if (Text[Position] == '}') { ++Position; return true; }
					if (Text[Position++] != ',') return false;
				}
			}

			bool ParseArray(JsonValue& Out, const int Depth)
			{
				Out.Type = JsonValue::Kind::Array;
				++Position;
				if (Position < Text.size() && Text[Position] == ']') { ++Position; return true; }
				for (;;)
				{
					JsonValue Value;
					if (!ParseValue(Value, Depth)) return false;
					Out.Array.push_back(std::move(Value));
					if (Position >= Text.size()) return false;
					if (Text[Position] == ']') { ++Position; return true; }
					if (Text[Position++] != ',') return false;
				}
			}

			bool ParseString(std::string& Out)
			{
				Out.clear();
				if (Position >= Text.size() || Text[Position++] != '"') return false;
				while (Position < Text.size())
				{
					const unsigned char Character = static_cast<unsigned char>(Text[Position++]);
					if (Character == '"') return true;
					if (Character < 0x20) return false;
					if (Character != '\\') { Out.push_back(static_cast<char>(Character)); continue; }
					if (Position >= Text.size()) return false;
					const char Escape = Text[Position++];
					switch (Escape)
					{
					case '"': Out.push_back('"'); break;
					case '\\': Out.push_back('\\'); break;
					case '/': Out.push_back('/'); break;
					case 'b': Out.push_back('\b'); break;
					case 'f': Out.push_back('\f'); break;
					case 'n': Out.push_back('\n'); break;
					case 'r': Out.push_back('\r'); break;
					case 't': Out.push_back('\t'); break;
					case 'u':
					{
						std::uint32_t CodePoint = 0;
						if (!ParseHex4(CodePoint)) return false;
						if (CodePoint >= 0xd800 && CodePoint <= 0xdbff)
						{
							if (Position + 2 > Text.size() || Text[Position++] != '\\' || Text[Position++] != 'u') return false;
							std::uint32_t Low = 0;
							if (!ParseHex4(Low) || Low < 0xdc00 || Low > 0xdfff) return false;
							CodePoint = 0x10000 + ((CodePoint - 0xd800) << 10) + (Low - 0xdc00);
						}
						else if (CodePoint >= 0xdc00 && CodePoint <= 0xdfff) return false;
						AppendUtf8CodePoint(Out, CodePoint);
						break;
					}
					default: return false;
					}
				}
				return false;
			}

			bool ParseHex4(std::uint32_t& Out)
			{
				if (Position + 4 > Text.size()) return false;
				Out = 0;
				for (int Index = 0; Index < 4; ++Index)
				{
					const char Character = Text[Position++];
					int Value = -1;
					if (Character >= '0' && Character <= '9') Value = Character - '0';
					else if (Character >= 'a' && Character <= 'f') Value = Character - 'a' + 10;
					else if (Character >= 'A' && Character <= 'F') Value = Character - 'A' + 10;
					if (Value < 0) return false;
					Out = (Out << 4) | static_cast<std::uint32_t>(Value);
				}
				return true;
			}

			bool ParseInteger(JsonValue& Out)
			{
				const std::size_t Begin = Position;
				if (Text[Position] == '-') ++Position;
				if (Position >= Text.size()) return false;
				if (Text[Position] == '0') ++Position;
				else
				{
					if (Text[Position] < '1' || Text[Position] > '9') return false;
					while (Position < Text.size() && Text[Position] >= '0' && Text[Position] <= '9') ++Position;
				}
				const std::string Token = Text.substr(Begin, Position - Begin);
				if (Token == "-0") { Out.Type=JsonValue::Kind::Integer; Out.Integer=0; return true; }
				bool Negative = Token[0] == '-';
				std::uint64_t Magnitude = 0;
				for (std::size_t Index = Negative ? 1U : 0U; Index < Token.size(); ++Index)
				{
					const unsigned Digit = static_cast<unsigned>(Token[Index] - '0');
					if (Magnitude > (static_cast<std::uint64_t>(MaxExactJsonInteger) - Digit) / 10U) return false;
					Magnitude = Magnitude * 10U + Digit;
				}
				Out.Type = JsonValue::Kind::Integer;
				Out.Integer = Negative ? -static_cast<std::int64_t>(Magnitude) : static_cast<std::int64_t>(Magnitude);
				return true;
			}

			bool Match(const char* Literal)
			{
				const std::size_t Size = std::strlen(Literal);
				if (Text.compare(Position, Size, Literal) != 0) return false;
				Position += Size;
				return true;
			}

			const std::string& Text;
			Error& ErrorRef;
			std::size_t Position = 0;
		};

		inline const JsonValue* Field(const JsonValue& Object, const char* Name)
		{
			if (Object.Type != JsonValue::Kind::Object) return nullptr;
			for (const auto& Entry : Object.Object) if (Entry.first == Name) return &Entry.second;
			return nullptr;
		}

		inline bool ExactFields(const JsonValue& Object, const std::initializer_list<const char*> Names)
		{
			if (Object.Type != JsonValue::Kind::Object || Object.Object.size() != Names.size()) return false;
			for (const char* Name : Names) if (!Field(Object, Name)) return false;
			return true;
		}

		inline void AppendJsonString(std::string& Out, const std::string& Value)
		{
			static constexpr char Hex[] = "0123456789abcdef";
			Out.push_back('"');
			for (const unsigned char Character : Value)
			{
				switch (Character)
				{
				case '"': Out += "\\\""; break;
				case '\\': Out += "\\\\"; break;
				case '\b': Out += "\\b"; break;
				case '\f': Out += "\\f"; break;
				case '\n': Out += "\\n"; break;
				case '\r': Out += "\\r"; break;
				case '\t': Out += "\\t"; break;
				default:
					if (Character < 0x20)
					{
						Out += "\\u00";
						Out.push_back(Hex[(Character >> 4) & 0x0f]);
						Out.push_back(Hex[Character & 0x0f]);
					}
					else Out.push_back(static_cast<char>(Character));
					break;
				}
			}
			Out.push_back('"');
		}
	}

	inline const char* CompletionText(const CompletionMode Value)
	{
		return Value == CompletionMode::Full ? "full" : "degraded";
	}

	inline const char* FreezeText(const FreezePolicy Value)
	{
		return Value == FreezePolicy::RequireFull ? "requireFull" : "allowDegraded";
	}

	inline bool MappingLess(const MatchedMapping& Left, const MatchedMapping& Right)
	{
		if (Left.RouteSlotIndex != Right.RouteSlotIndex) return Left.RouteSlotIndex < Right.RouteSlotIndex;
		return Left.DroneId < Right.DroneId;
	}

	inline bool SerializeManifestInternal(const Manifest& Value, const bool IncludeDigest, std::string& Out)
	{
		using detail::AppendJsonString;
		std::vector<MatchedMapping> Mapping = Value.Mapping;
		std::sort(Mapping.begin(), Mapping.end(), MappingLess);
		std::vector<RouteRecord> Routes = Value.Routes;
		std::sort(Routes.begin(), Routes.end(), [](const RouteRecord& Left, const RouteRecord& Right)
		{
			return Left.RouteSlotIndex < Right.RouteSlotIndex;
		});
		Out.clear();
		Out.reserve(1024 + Mapping.size() * 64 + Routes.size() * 180);
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
		Key("assignment"); Out.push_back('{');
		StringField("fleetSessionId", Value.Assignment.FleetSessionId); Out.push_back(',');
		StringField("assignmentAuthoritySessionId", Value.Assignment.AssignmentAuthoritySessionId); Out.push_back(',');
		IntegerField("assignmentRevision", Value.Assignment.AssignmentRevision); Out.push_back(',');
		StringField("assignmentDigest", DigestToLowerHex(Value.Assignment.AssignmentDigest)); Out += "},";
		Key("assignmentAudit"); Out.push_back('{');
		IntegerField("fleetRevisionAtCapture", Value.FleetRevisionAtCapture); Out.push_back(',');
		IntegerField("routeDatasetGeneration", Value.RouteDatasetGeneration); Out.push_back(',');
		StringField("completionMode", CompletionText(Value.Completion)); Out.push_back(',');
		StringField("freezePolicy", FreezeText(Value.Freeze)); Out.push_back(',');
		IntegerField("matchedCount", Value.MatchedCount); Out.push_back(',');
		IntegerField("spareCount", Value.SpareCount); Out.push_back(',');
		IntegerField("excludedCount", Value.ExcludedCount); Out.push_back(',');
		IntegerField("vacantCount", Value.VacantCount); Out += "},";
		Key("source"); Out.push_back('{');
		StringField("sourceDatasetDigest", DigestToLowerHex(Value.Source.SourceDatasetDigest)); Out.push_back(',');
		IntegerField("sourceByteSize", Value.Source.SourceByteSize); Out.push_back(',');
		StringField("sourceDisplayFileName", Value.Source.SourceDisplayFileName); Out.push_back(',');
		IntegerField("frameCount", Value.FrameCount); Out.push_back(',');
		IntegerField("routeSlotCount", Value.RouteSlotCount); Out += "},";
		Key("range"); Out.push_back('{'); Key("hasRange"); Out += Value.HasRange ? "true," : "false,";
		Key("start"); Out += Value.HasRange ? std::to_string(Value.RangeStart) : "null"; Out.push_back(',');
		Key("end"); Out += Value.HasRange ? std::to_string(Value.RangeEnd) : "null"; Out += "},";
		Key("generator"); Out.push_back('{');
		StringField("waypointGeneratorVersion", Value.WaypointGeneratorVersion); Out.push_back(',');
		StringField("generatorInputDigest", DigestToLowerHex(Value.GeneratorInputDigest)); Out += "},";
		Key("mapping"); Out.push_back('[');
		for (std::size_t Index = 0; Index < Mapping.size(); ++Index)
		{
			if (Index) Out.push_back(',');
			Out.push_back('{'); StringField("droneId", Mapping[Index].DroneId); Out.push_back(',');
			IntegerField("routeSlotIndex", Mapping[Index].RouteSlotIndex); Out.push_back('}');
		}
		Out += "],";
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
		if (IncludeDigest) { Out.push_back(','); StringField("artifactSetDigest", DigestToLowerHex(Value.ArtifactSetDigest)); }
		Out.push_back('}');
		return true;
	}

	inline BuildIdentity BuildIdentityFromManifest(const Manifest& Value)
	{
		BuildIdentity Result;
		Result.SchemaVersion = Value.SchemaVersion;
		Result.Assignment = Value.Assignment;
		Result.SourceDatasetDigest = Value.Source.SourceDatasetDigest;
		Result.WaypointGeneratorVersion = Value.WaypointGeneratorVersion;
		Result.GeneratorInputDigest = Value.GeneratorInputDigest;
		return Result;
	}

	inline bool ComputeArtifactSetDigest(const Manifest& Value, Digest256& Out, Error& ErrorValue)
	{
		if (!Value.Assignment.AssignmentDigest.IsSet || !Value.Source.SourceDatasetDigest.IsSet
			|| !Value.GeneratorInputDigest.IsSet)
			return Fail(ErrorValue, ErrorCode::InvalidDigest, "Manifest digest inputs are invalid.");
		for (const RouteRecord& Route : Value.Routes) if (!Route.RouteArtifactDigest.IsSet)
			return Fail(ErrorValue, ErrorCode::InvalidDigest, "Route digest is invalid.");
		std::string Bytes;
		SerializeManifestInternal(Value, false, Bytes);
		if (!Sha256::Hash(Bytes.data(), Bytes.size(), Out))
			return Fail(ErrorValue, ErrorCode::DigestMismatch, "ArtifactSetDigest SHA-256 failed.");
		return true;
	}

	inline bool ValidateManifest(const Manifest& Value, Error& ErrorValue)
	{
		if (Value.SchemaVersion != ArtifactSchemaVersion || !ValidateArtifactSetId(Value.ArtifactSetId, ErrorValue)
			|| Value.CreatedAtUtc <= 0 || Value.CreatedAtUtc > MaxExactJsonInteger
			|| !ValidateAssignmentBinding(Value.Assignment, ErrorValue))
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest identity is invalid.");
		if (Value.FleetRevisionAtCapture < 0 || Value.RouteDatasetGeneration < 0
			|| Value.MatchedCount <= 0 || Value.SpareCount < 0 || Value.ExcludedCount < 0 || Value.VacantCount < 0
			|| Value.RouteSlotCount <= 0 || Value.FrameCount <= 0 || Value.MatchedCount > Value.RouteSlotCount
			|| Value.MatchedCount + Value.VacantCount != Value.RouteSlotCount)
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest counts are invalid.");
		if (!Value.Source.SourceDatasetDigest.IsSet || Value.Source.SourceByteSize < 0
			|| !IsCanonicalString(Value.Source.SourceDisplayFileName)
			|| Value.Source.SourceDisplayFileName == "." || Value.Source.SourceDisplayFileName == ".."
			|| Value.Source.SourceDisplayFileName.find('/') != std::string::npos
			|| Value.Source.SourceDisplayFileName.find('\\') != std::string::npos)
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest source identity is invalid.");
		if (Value.Completion == CompletionMode::Full)
		{
			if (Value.VacantCount != 0 || Value.MatchedCount != Value.RouteSlotCount)
				return Fail(ErrorValue, ErrorCode::InvalidManifest, "Full completion counts are invalid.");
		}
		else if (Value.VacantCount <= 0 || Value.MatchedCount >= Value.RouteSlotCount
			|| Value.Freeze != FreezePolicy::AllowDegraded)
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Degraded completion counts are invalid.");
		if (!IsCanonicalString(Value.WaypointGeneratorVersion) || !Value.GeneratorInputDigest.IsSet)
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest generator identity is invalid.");
		if ((Value.HasRange && (Value.RangeStart < 0 || Value.RangeEnd < Value.RangeStart || Value.RangeEnd >= Value.FrameCount))
			|| (!Value.HasRange && (Value.RangeStart != 0 || Value.RangeEnd != 0)))
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest range is invalid.");
		if (Value.Mapping.size() != static_cast<std::size_t>(Value.MatchedCount))
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest mapping count is invalid.");
		std::set<std::string> DroneIds;
		std::set<std::int32_t> Slots;
		for (std::size_t Index = 0; Index < Value.Mapping.size(); ++Index)
		{
			const MatchedMapping& Entry = Value.Mapping[Index];
			if (!IsCanonicalString(Entry.DroneId) || Entry.RouteSlotIndex < 0 || Entry.RouteSlotIndex >= Value.RouteSlotCount
				|| !DroneIds.insert(Entry.DroneId).second || !Slots.insert(Entry.RouteSlotIndex).second
				|| (Index > 0 && !MappingLess(Value.Mapping[Index - 1], Entry)))
				return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest mapping is invalid or noncanonical.");
		}
		if (Value.Routes.size() != static_cast<std::size_t>(Value.RouteSlotCount))
			return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest does not cover the complete route domain.");
		for (std::int32_t Slot = 0; Slot < Value.RouteSlotCount; ++Slot)
		{
			std::string Expected;
			if (!BuildRouteRelativePath(Slot, Expected, ErrorValue)) return false;
			const RouteRecord& Route = Value.Routes[static_cast<std::size_t>(Slot)];
			if (Route.RouteSlotIndex != Slot || Route.RelativePath != Expected || Route.FileByteSize < 0
				|| !Route.RouteArtifactDigest.IsSet)
				return Fail(ErrorValue, ErrorCode::InvalidManifest, "Manifest route descriptor is invalid.");
		}
		std::string ExpectedSetId;
		if (!ComputeArtifactSetId(BuildIdentityFromManifest(Value), ExpectedSetId, ErrorValue)
			|| ExpectedSetId != Value.ArtifactSetId)
			return Fail(ErrorValue, ErrorCode::InvalidIdentity, "Manifest ArtifactSetID mismatch.");
		Digest256 ExpectedDigest;
		if (!Value.ArtifactSetDigest.IsSet || !ComputeArtifactSetDigest(Value, ExpectedDigest, ErrorValue)
			|| ExpectedDigest != Value.ArtifactSetDigest)
			return Fail(ErrorValue, ErrorCode::DigestMismatch, "Manifest ArtifactSetDigest mismatch.");
		ErrorValue = {};
		return true;
	}

	inline bool SerializeCanonicalManifest(const Manifest& Value, std::string& Out, Error& ErrorValue)
	{
		Out.clear();
		if (!ValidateManifest(Value, ErrorValue)) return false;
		return SerializeManifestInternal(Value, true, Out);
	}

	inline bool ParseCanonicalManifest(const std::string& Bytes, Manifest& Out, Error& ErrorValue)
	{
		Out = {};
		if (Bytes.empty() || (Bytes.size() >= 3 && static_cast<unsigned char>(Bytes[0]) == 0xef
			&& static_cast<unsigned char>(Bytes[1]) == 0xbb && static_cast<unsigned char>(Bytes[2]) == 0xbf))
			return Fail(ErrorValue, ErrorCode::MalformedJson, "Manifest must be non-empty UTF-8 without BOM.");
		detail::JsonValue Root;
		if (!detail::JsonParser(Bytes, ErrorValue).Parse(Root)) return false;
		if (!detail::ExactFields(Root, {"artifactSchemaVersion","artifactSetId","createdAtUtc","assignment",
			"assignmentAudit","source","range","generator","mapping","routes","artifactSetDigest"}))
			return Fail(ErrorValue, ErrorCode::UnknownOrMissingField, "Manifest has missing or unknown fields.");
		auto String = [&ErrorValue](const detail::JsonValue* Value, std::string& Target)
		{
			if (!Value || Value->Type != detail::JsonValue::Kind::String)
				return Fail(ErrorValue, ErrorCode::WrongFieldType, "Manifest field must be a string.");
			Target = Value->String; return true;
		};
		auto Integer = [&ErrorValue](const detail::JsonValue* Value, std::int64_t& Target)
		{
			if (!Value || Value->Type != detail::JsonValue::Kind::Integer)
				return Fail(ErrorValue, ErrorCode::WrongFieldType, "Manifest field must be an integer.");
			Target = Value->Integer; return true;
		};
		auto Int32 = [&Integer, &ErrorValue](const detail::JsonValue* Value, std::int32_t& Target)
		{
			std::int64_t Number = 0;
			if (!Integer(Value, Number) || Number < std::numeric_limits<std::int32_t>::min()
				|| Number > std::numeric_limits<std::int32_t>::max())
				return Fail(ErrorValue, ErrorCode::IntegerOutOfRange, "Manifest integer is outside int32 range.");
			Target = static_cast<std::int32_t>(Number); return true;
		};
		std::int32_t Schema = 0;
		if (!Int32(detail::Field(Root,"artifactSchemaVersion"),Schema) || Schema < 0 || Schema > 65535
			|| !String(detail::Field(Root,"artifactSetId"),Out.ArtifactSetId)
			|| !Integer(detail::Field(Root,"createdAtUtc"),Out.CreatedAtUtc)) return false;
		Out.SchemaVersion = static_cast<std::uint16_t>(Schema);
		const detail::JsonValue* Assignment = detail::Field(Root,"assignment");
		if (!Assignment || !detail::ExactFields(*Assignment,{"fleetSessionId","assignmentAuthoritySessionId","assignmentRevision","assignmentDigest"}))
			return Fail(ErrorValue,ErrorCode::UnknownOrMissingField,"assignment has missing or unknown fields.");
		std::string DigestText;
		if (!String(detail::Field(*Assignment,"fleetSessionId"),Out.Assignment.FleetSessionId)
			|| !String(detail::Field(*Assignment,"assignmentAuthoritySessionId"),Out.Assignment.AssignmentAuthoritySessionId)
			|| !Integer(detail::Field(*Assignment,"assignmentRevision"),Out.Assignment.AssignmentRevision)
			|| !String(detail::Field(*Assignment,"assignmentDigest"),DigestText)
			|| !ParseLowerHexDigest(DigestText,Out.Assignment.AssignmentDigest,ErrorValue)) return false;
		const detail::JsonValue* Audit = detail::Field(Root,"assignmentAudit");
		if (!Audit || !detail::ExactFields(*Audit,{"fleetRevisionAtCapture","routeDatasetGeneration","completionMode","freezePolicy","matchedCount","spareCount","excludedCount","vacantCount"}))
			return Fail(ErrorValue,ErrorCode::UnknownOrMissingField,"assignmentAudit has missing or unknown fields.");
		std::string CompletionValue, FreezeValue;
		if (!Integer(detail::Field(*Audit,"fleetRevisionAtCapture"),Out.FleetRevisionAtCapture)
			|| !Integer(detail::Field(*Audit,"routeDatasetGeneration"),Out.RouteDatasetGeneration)
			|| !String(detail::Field(*Audit,"completionMode"),CompletionValue)
			|| !String(detail::Field(*Audit,"freezePolicy"),FreezeValue)
			|| !Int32(detail::Field(*Audit,"matchedCount"),Out.MatchedCount)
			|| !Int32(detail::Field(*Audit,"spareCount"),Out.SpareCount)
			|| !Int32(detail::Field(*Audit,"excludedCount"),Out.ExcludedCount)
			|| !Int32(detail::Field(*Audit,"vacantCount"),Out.VacantCount)) return false;
		if (CompletionValue == "full") Out.Completion=CompletionMode::Full;
		else if (CompletionValue == "degraded") Out.Completion=CompletionMode::Degraded;
		else return Fail(ErrorValue,ErrorCode::InvalidManifest,"Unsupported completion mode.");
		if (FreezeValue == "requireFull") Out.Freeze=FreezePolicy::RequireFull;
		else if (FreezeValue == "allowDegraded") Out.Freeze=FreezePolicy::AllowDegraded;
		else return Fail(ErrorValue,ErrorCode::InvalidManifest,"Unsupported freeze policy.");
		const detail::JsonValue* Source = detail::Field(Root,"source");
		if (!Source || !detail::ExactFields(*Source,{"sourceDatasetDigest","sourceByteSize","sourceDisplayFileName","frameCount","routeSlotCount"}))
			return Fail(ErrorValue,ErrorCode::UnknownOrMissingField,"source has missing or unknown fields.");
		if (!String(detail::Field(*Source,"sourceDatasetDigest"),DigestText)
			|| !ParseLowerHexDigest(DigestText,Out.Source.SourceDatasetDigest,ErrorValue)
			|| !Integer(detail::Field(*Source,"sourceByteSize"),Out.Source.SourceByteSize)
			|| !String(detail::Field(*Source,"sourceDisplayFileName"),Out.Source.SourceDisplayFileName)
			|| !Int32(detail::Field(*Source,"frameCount"),Out.FrameCount)
			|| !Int32(detail::Field(*Source,"routeSlotCount"),Out.RouteSlotCount)) return false;
		const detail::JsonValue* Range = detail::Field(Root,"range");
		if (!Range || !detail::ExactFields(*Range,{"hasRange","start","end"}))
			return Fail(ErrorValue,ErrorCode::UnknownOrMissingField,"range has missing or unknown fields.");
		const detail::JsonValue* HasRange = detail::Field(*Range,"hasRange");
		if (!HasRange || HasRange->Type != detail::JsonValue::Kind::Bool)
			return Fail(ErrorValue,ErrorCode::WrongFieldType,"hasRange must be boolean.");
		Out.HasRange = HasRange->Boolean;
		if (Out.HasRange)
		{
			if (!Int32(detail::Field(*Range,"start"),Out.RangeStart) || !Int32(detail::Field(*Range,"end"),Out.RangeEnd)) return false;
		}
		else
		{
			const auto* Start=detail::Field(*Range,"start"); const auto* End=detail::Field(*Range,"end");
			if (!Start || !End || Start->Type!=detail::JsonValue::Kind::Null || End->Type!=detail::JsonValue::Kind::Null)
				return Fail(ErrorValue,ErrorCode::WrongFieldType,"Absent range must use null bounds.");
		}
		const detail::JsonValue* Generator = detail::Field(Root,"generator");
		if (!Generator || !detail::ExactFields(*Generator,{"waypointGeneratorVersion","generatorInputDigest"}))
			return Fail(ErrorValue,ErrorCode::UnknownOrMissingField,"generator has missing or unknown fields.");
		if (!String(detail::Field(*Generator,"waypointGeneratorVersion"),Out.WaypointGeneratorVersion)
			|| !String(detail::Field(*Generator,"generatorInputDigest"),DigestText)
			|| !ParseLowerHexDigest(DigestText,Out.GeneratorInputDigest,ErrorValue)) return false;
		const detail::JsonValue* Mapping = detail::Field(Root,"mapping");
		if (!Mapping || Mapping->Type != detail::JsonValue::Kind::Array)
			return Fail(ErrorValue,ErrorCode::WrongFieldType,"mapping must be an array.");
		Out.Mapping.reserve(Mapping->Array.size());
		for (const detail::JsonValue& Entry : Mapping->Array)
		{
			if (!detail::ExactFields(Entry,{"droneId","routeSlotIndex"}))
				return Fail(ErrorValue,ErrorCode::UnknownOrMissingField,"mapping entry has missing or unknown fields.");
			MatchedMapping Parsed;
			if (!String(detail::Field(Entry,"droneId"),Parsed.DroneId)
				|| !Int32(detail::Field(Entry,"routeSlotIndex"),Parsed.RouteSlotIndex)) return false;
			Out.Mapping.push_back(std::move(Parsed));
		}
		const detail::JsonValue* Routes = detail::Field(Root,"routes");
		if (!Routes || Routes->Type != detail::JsonValue::Kind::Array)
			return Fail(ErrorValue,ErrorCode::WrongFieldType,"routes must be an array.");
		Out.Routes.reserve(Routes->Array.size());
		for (const detail::JsonValue& Entry : Routes->Array)
		{
			if (!detail::ExactFields(Entry,{"routeSlotIndex","relativePath","fileByteSize","routeArtifactDigest"}))
				return Fail(ErrorValue,ErrorCode::UnknownOrMissingField,"route entry has missing or unknown fields.");
			RouteRecord Parsed;
			if (!Int32(detail::Field(Entry,"routeSlotIndex"),Parsed.RouteSlotIndex)
				|| !String(detail::Field(Entry,"relativePath"),Parsed.RelativePath)
				|| !Integer(detail::Field(Entry,"fileByteSize"),Parsed.FileByteSize)
				|| !String(detail::Field(Entry,"routeArtifactDigest"),DigestText)
				|| !ParseLowerHexDigest(DigestText,Parsed.RouteArtifactDigest,ErrorValue)) return false;
			Out.Routes.push_back(std::move(Parsed));
		}
		if (!String(detail::Field(Root,"artifactSetDigest"),DigestText)
			|| !ParseLowerHexDigest(DigestText,Out.ArtifactSetDigest,ErrorValue)
			|| !ValidateManifest(Out,ErrorValue)) return false;
		std::string Canonical;
		SerializeManifestInternal(Out,true,Canonical);
		if (Canonical != Bytes)
			return Fail(ErrorValue,ErrorCode::NonCanonicalManifest,"Manifest bytes are not canonical.");
		return true;
	}

	inline bool SerializeReadyMarker(
		const std::string& ArtifactSetId,
		const Digest256& ArtifactSetDigest,
		std::string& Out,
		Error& ErrorValue)
	{
		Out.clear();
		if (!ValidateArtifactSetId(ArtifactSetId,ErrorValue) || !ArtifactSetDigest.IsSet)
			return Fail(ErrorValue,ErrorCode::InvalidIdentity,"READY identity is invalid.");
		Out = "DRONEGCS_ROUTE_ARTIFACT_READY_V1\nArtifactSetID=" + ArtifactSetId
			+ "\nArtifactSetDigest=" + DigestToLowerHex(ArtifactSetDigest) + "\n";
		return true;
	}

	inline bool ParseReadyMarker(
		const std::string& Bytes,
		std::string& OutArtifactSetId,
		Digest256& OutArtifactSetDigest,
		Error& ErrorValue)
	{
		OutArtifactSetId.clear(); OutArtifactSetDigest = {};
		if (Bytes.empty() || (Bytes.size() >= 3 && static_cast<unsigned char>(Bytes[0]) == 0xef
			&& static_cast<unsigned char>(Bytes[1]) == 0xbb && static_cast<unsigned char>(Bytes[2]) == 0xbf))
			return Fail(ErrorValue,ErrorCode::MalformedReady,"READY must be non-empty UTF-8/ASCII without BOM.");
		const std::string Prefix="DRONEGCS_ROUTE_ARTIFACT_READY_V1\nArtifactSetID=";
		const std::string Middle="\nArtifactSetDigest=";
		if (Bytes.rfind(Prefix,0)!=0 || Bytes.size()<Prefix.size()+64+Middle.size()+64+1)
			return Fail(ErrorValue,ErrorCode::MalformedReady,"READY format is malformed.");
		const std::size_t MiddleAt=Prefix.size()+64;
		if (Bytes.compare(MiddleAt,Middle.size(),Middle)!=0 || Bytes.back()!='\n')
			return Fail(ErrorValue,ErrorCode::MalformedReady,"READY format is malformed.");
		OutArtifactSetId=Bytes.substr(Prefix.size(),64);
		const std::string DigestText=Bytes.substr(MiddleAt+Middle.size(),64);
		std::string Canonical;
		if (!ValidateArtifactSetId(OutArtifactSetId,ErrorValue)
			|| !ParseLowerHexDigest(DigestText,OutArtifactSetDigest,ErrorValue)
			|| !SerializeReadyMarker(OutArtifactSetId,OutArtifactSetDigest,Canonical,ErrorValue)) return false;
		if (Canonical!=Bytes) return Fail(ErrorValue,ErrorCode::NonCanonicalReady,"READY bytes are not canonical.");
		return true;
	}
}
