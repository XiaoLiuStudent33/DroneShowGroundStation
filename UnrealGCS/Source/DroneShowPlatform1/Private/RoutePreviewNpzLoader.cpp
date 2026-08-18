#include "RoutePreviewNpzLoader.h"

#include "HAL/PlatformFileManager.h"
#include "Math/Float16.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RouteArtifactTypes.h"
#include "RoutePreviewData.h"
#include "Internationalization/Regex.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

namespace DroneGCSRoutePreviewNpzPrivate
{
	constexpr uint32 LocalHeaderSignature = 0x04034b50;
	constexpr uint32 CentralHeaderSignature = 0x02014b50;
	constexpr uint32 EndOfCentralDirectorySignature = 0x06054b50;
	constexpr uint16 ZipStored = 0;
	constexpr uint16 ZipDeflate = 8;

	uint16 ReadU16(const uint8* Data)
	{
		return static_cast<uint16>(Data[0])
			| (static_cast<uint16>(Data[1]) << 8);
	}

	uint32 ReadU32(const uint8* Data)
	{
		return static_cast<uint32>(Data[0])
			| (static_cast<uint32>(Data[1]) << 8)
			| (static_cast<uint32>(Data[2]) << 16)
			| (static_cast<uint32>(Data[3]) << 24);
	}

	bool IsRangeValid(const int64 Offset, const int64 Size, const int64 TotalSize)
	{
		return Offset >= 0 && Size >= 0 && Offset <= TotalSize && Size <= TotalSize - Offset;
	}

	struct FZipEntry
	{
		FString Name;
		uint16 Flags = 0;
		uint16 CompressionMethod = 0;
		uint32 Crc32 = 0;
		uint32 CompressedSize = 0;
		uint32 UncompressedSize = 0;
		uint32 LocalHeaderOffset = 0;
	};

	bool ReadZipDirectory(const TArray<uint8>& Archive, TArray<FZipEntry>& OutEntries, FString& OutError)
	{
		constexpr int64 MinimumEocdSize = 22;
		constexpr int64 MaximumCommentSize = 65535;
		if (Archive.Num() < MinimumEocdSize)
		{
			OutError = TEXT("NPZ is too small to contain a ZIP directory.");
			return false;
		}

		const int64 SearchStart = FMath::Max<int64>(0, Archive.Num() - MinimumEocdSize - MaximumCommentSize);
		int64 EocdOffset = INDEX_NONE;
		for (int64 Offset = Archive.Num() - MinimumEocdSize; Offset >= SearchStart; --Offset)
		{
			if (ReadU32(Archive.GetData() + Offset) == EndOfCentralDirectorySignature)
			{
				EocdOffset = Offset;
				break;
			}
		}
		if (EocdOffset == INDEX_NONE)
		{
			OutError = TEXT("NPZ ZIP end-of-central-directory record was not found.");
			return false;
		}

		const uint8* Eocd = Archive.GetData() + EocdOffset;
		const uint16 DiskNumber = ReadU16(Eocd + 4);
		const uint16 DirectoryDisk = ReadU16(Eocd + 6);
		const uint16 EntriesOnDisk = ReadU16(Eocd + 8);
		const uint16 EntryCount = ReadU16(Eocd + 10);
		const uint32 DirectorySize = ReadU32(Eocd + 12);
		const uint32 DirectoryOffset = ReadU32(Eocd + 16);
		if (DiskNumber != 0 || DirectoryDisk != 0 || EntriesOnDisk != EntryCount)
		{
			OutError = TEXT("Multi-disk NPZ ZIP archives are not supported.");
			return false;
		}
		if (EntryCount == MAX_uint16 || DirectorySize == MAX_uint32 || DirectoryOffset == MAX_uint32)
		{
			OutError = TEXT("ZIP64 NPZ archives are not supported.");
			return false;
		}
		if (!IsRangeValid(DirectoryOffset, DirectorySize, Archive.Num()))
		{
			OutError = TEXT("NPZ central directory points outside the file.");
			return false;
		}

		int64 Cursor = DirectoryOffset;
		OutEntries.Reserve(EntryCount);
		for (uint16 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
		{
			constexpr int64 FixedCentralHeaderSize = 46;
			if (!IsRangeValid(Cursor, FixedCentralHeaderSize, Archive.Num())
				|| ReadU32(Archive.GetData() + Cursor) != CentralHeaderSignature)
			{
				OutError = FString::Printf(TEXT("Invalid ZIP central directory entry %u."), EntryIndex);
				return false;
			}

			const uint8* Header = Archive.GetData() + Cursor;
			const uint16 NameLength = ReadU16(Header + 28);
			const uint16 ExtraLength = ReadU16(Header + 30);
			const uint16 CommentLength = ReadU16(Header + 32);
			const int64 FullHeaderSize = FixedCentralHeaderSize + NameLength + ExtraLength + CommentLength;
			if (!IsRangeValid(Cursor, FullHeaderSize, Archive.Num()))
			{
				OutError = FString::Printf(TEXT("Truncated ZIP central directory entry %u."), EntryIndex);
				return false;
			}

			FZipEntry& Entry = OutEntries.AddDefaulted_GetRef();
			Entry.Flags = ReadU16(Header + 8);
			Entry.CompressionMethod = ReadU16(Header + 10);
			Entry.Crc32 = ReadU32(Header + 16);
			Entry.CompressedSize = ReadU32(Header + 20);
			Entry.UncompressedSize = ReadU32(Header + 24);
			Entry.LocalHeaderOffset = ReadU32(Header + 42);
			FUTF8ToTCHAR ConvertedName(reinterpret_cast<const ANSICHAR*>(Header + 46), NameLength);
			Entry.Name = FString(ConvertedName.Length(), ConvertedName.Get());
			Cursor += FullHeaderSize;
		}
		return true;
	}

	bool InflateRawDeflate(
		const uint8* CompressedData,
		const uint32 CompressedSize,
		TArray<uint8>& OutData,
		const uint32 UncompressedSize,
		FString& OutError)
	{
		OutData.SetNumUninitialized(static_cast<int32>(UncompressedSize));
		z_stream Stream{};
		Stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(CompressedData));
		Stream.avail_in = CompressedSize;
		Stream.next_out = reinterpret_cast<Bytef*>(OutData.GetData());
		Stream.avail_out = UncompressedSize;
		const int32 InitResult = inflateInit2(&Stream, -MAX_WBITS);
		if (InitResult != Z_OK)
		{
			OutError = FString::Printf(TEXT("zlib initialization failed (%d)."), InitResult);
			return false;
		}

		const int32 InflateResult = inflate(&Stream, Z_FINISH);
		inflateEnd(&Stream);
		if (InflateResult != Z_STREAM_END || Stream.total_out != UncompressedSize)
		{
			OutError = FString::Printf(
				TEXT("NPZ deflate failed (%d, produced %lu of %u bytes)."),
				InflateResult,
				static_cast<unsigned long>(Stream.total_out),
				UncompressedSize);
			return false;
		}
		return true;
	}

	bool ExtractZipEntry(
		const TArray<uint8>& Archive,
		const FZipEntry& Entry,
		TArray<uint8>& OutData,
		FString& OutError)
	{
		if ((Entry.Flags & 0x0001) != 0)
		{
			OutError = FString::Printf(TEXT("Encrypted NPZ entry is not supported: %s"), *Entry.Name);
			return false;
		}
		if (Entry.UncompressedSize > static_cast<uint32>(MAX_int32)
			|| Entry.CompressedSize > static_cast<uint32>(MAX_int32))
		{
			OutError = FString::Printf(TEXT("NPZ entry exceeds UE array limits: %s"), *Entry.Name);
			return false;
		}

		constexpr int64 FixedLocalHeaderSize = 30;
		if (!IsRangeValid(Entry.LocalHeaderOffset, FixedLocalHeaderSize, Archive.Num()))
		{
			OutError = FString::Printf(TEXT("Invalid local ZIP header for %s."), *Entry.Name);
			return false;
		}
		const uint8* LocalHeader = Archive.GetData() + Entry.LocalHeaderOffset;
		if (ReadU32(LocalHeader) != LocalHeaderSignature)
		{
			OutError = FString::Printf(TEXT("Local ZIP signature mismatch for %s."), *Entry.Name);
			return false;
		}
		const uint16 NameLength = ReadU16(LocalHeader + 26);
		const uint16 ExtraLength = ReadU16(LocalHeader + 28);
		const int64 DataOffset = static_cast<int64>(Entry.LocalHeaderOffset)
			+ FixedLocalHeaderSize + NameLength + ExtraLength;
		if (!IsRangeValid(DataOffset, Entry.CompressedSize, Archive.Num()))
		{
			OutError = FString::Printf(TEXT("Compressed payload is truncated for %s."), *Entry.Name);
			return false;
		}

		const uint8* CompressedData = Archive.GetData() + DataOffset;
		if (Entry.CompressionMethod == ZipStored)
		{
			if (Entry.CompressedSize != Entry.UncompressedSize)
			{
				OutError = FString::Printf(TEXT("Stored ZIP entry has inconsistent sizes: %s"), *Entry.Name);
				return false;
			}
			OutData.SetNumUninitialized(static_cast<int32>(Entry.UncompressedSize));
			FMemory::Memcpy(OutData.GetData(), CompressedData, Entry.UncompressedSize);
		}
		else if (Entry.CompressionMethod == ZipDeflate)
		{
			if (!InflateRawDeflate(CompressedData, Entry.CompressedSize, OutData, Entry.UncompressedSize, OutError))
			{
				OutError = FString::Printf(TEXT("%s Entry=%s"), *OutError, *Entry.Name);
				return false;
			}
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Unsupported ZIP compression method %u for %s."),
				Entry.CompressionMethod,
				*Entry.Name);
			return false;
		}

		if (FCrc::MemCrc32(OutData.GetData(), OutData.Num()) != Entry.Crc32)
		{
			OutError = FString::Printf(TEXT("CRC mismatch while reading %s."), *Entry.Name);
			return false;
		}
		return true;
	}

	enum class ENpyValueType : uint8
	{
		Float16,
		Float32,
		Float64,
		Int32,
		Int64
	};

	struct FNpyArrayView
	{
		ENpyValueType ValueType = ENpyValueType::Float32;
		int32 ElementSize = 0;
		TArray<int64> Shape;
		const uint8* Data = nullptr;
		int64 ElementCount = 0;
	};

	bool ParseShape(const FString& ShapeText, TArray<int64>& OutShape)
	{
		TArray<FString> Parts;
		ShapeText.ParseIntoArray(Parts, TEXT(","), true);
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (Part.IsEmpty())
			{
				continue;
			}
			int64 Dimension = 0;
			if (!LexTryParseString(Dimension, *Part) || Dimension <= 0)
			{
				return false;
			}
			OutShape.Add(Dimension);
		}
		return !OutShape.IsEmpty();
	}

	bool ParseNpy(const FString& Name, const TArray<uint8>& Bytes, FNpyArrayView& OutView, FString& OutError)
	{
		const uint8 Magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
		if (Bytes.Num() < 10 || FMemory::Memcmp(Bytes.GetData(), Magic, UE_ARRAY_COUNT(Magic)) != 0)
		{
			OutError = FString::Printf(TEXT("%s is not a valid NPY payload."), *Name);
			return false;
		}

		const uint8 Major = Bytes[6];
		int64 HeaderOffset = 0;
		uint32 HeaderLength = 0;
		if (Major == 1)
		{
			HeaderOffset = 10;
			HeaderLength = ReadU16(Bytes.GetData() + 8);
		}
		else if (Major == 2 || Major == 3)
		{
			if (Bytes.Num() < 12)
			{
				OutError = FString::Printf(TEXT("Truncated NPY v%u header in %s."), Major, *Name);
				return false;
			}
			HeaderOffset = 12;
			HeaderLength = ReadU32(Bytes.GetData() + 8);
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported NPY version %u in %s."), Major, *Name);
			return false;
		}
		if (!IsRangeValid(HeaderOffset, HeaderLength, Bytes.Num()))
		{
			OutError = FString::Printf(TEXT("Truncated NPY header in %s."), *Name);
			return false;
		}

		FUTF8ToTCHAR ConvertedHeader(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + HeaderOffset),
			HeaderLength);
		const FString Header(ConvertedHeader.Length(), ConvertedHeader.Get());
		FRegexMatcher DescrMatcher(FRegexPattern(TEXT("['\"]descr['\"]\\s*:\\s*['\"]([^'\"]+)['\"]")), Header);
		FRegexMatcher FortranMatcher(FRegexPattern(TEXT("['\"]fortran_order['\"]\\s*:\\s*(True|False)")), Header);
		FRegexMatcher ShapeMatcher(FRegexPattern(TEXT("['\"]shape['\"]\\s*:\\s*\\(([^)]*)\\)")), Header);
		if (!DescrMatcher.FindNext() || !FortranMatcher.FindNext() || !ShapeMatcher.FindNext())
		{
			OutError = FString::Printf(TEXT("Required NPY header fields are missing in %s."), *Name);
			return false;
		}
		if (FortranMatcher.GetCaptureGroup(1) != TEXT("False"))
		{
			OutError = FString::Printf(TEXT("Fortran-order NPY arrays are not supported: %s"), *Name);
			return false;
		}
		if (!ParseShape(ShapeMatcher.GetCaptureGroup(1), OutView.Shape))
		{
			OutError = FString::Printf(TEXT("Invalid NPY shape in %s."), *Name);
			return false;
		}

		const FString Descr = DescrMatcher.GetCaptureGroup(1);
		if (Descr.StartsWith(TEXT(">")))
		{
			OutError = FString::Printf(TEXT("Big-endian NPY data is not supported: %s (%s)"), *Name, *Descr);
			return false;
		}
		const FString TypeCode = Descr.Mid(1);
		if (TypeCode == TEXT("f2"))
		{
			OutView.ValueType = ENpyValueType::Float16;
			OutView.ElementSize = 2;
		}
		else if (TypeCode == TEXT("f4"))
		{
			OutView.ValueType = ENpyValueType::Float32;
			OutView.ElementSize = 4;
		}
		else if (TypeCode == TEXT("f8"))
		{
			OutView.ValueType = ENpyValueType::Float64;
			OutView.ElementSize = 8;
		}
		else if (TypeCode == TEXT("i4"))
		{
			OutView.ValueType = ENpyValueType::Int32;
			OutView.ElementSize = 4;
		}
		else if (TypeCode == TEXT("i8"))
		{
			OutView.ValueType = ENpyValueType::Int64;
			OutView.ElementSize = 8;
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported NPY dtype in %s: %s"), *Name, *Descr);
			return false;
		}

		int64 ElementCount = 1;
		for (const int64 Dimension : OutView.Shape)
		{
			if (ElementCount > MAX_int64 / Dimension)
			{
				OutError = FString::Printf(TEXT("NPY shape overflows in %s."), *Name);
				return false;
			}
			ElementCount *= Dimension;
		}
		const int64 DataOffset = HeaderOffset + HeaderLength;
		const int64 RequiredBytes = ElementCount * OutView.ElementSize;
		if (!IsRangeValid(DataOffset, RequiredBytes, Bytes.Num()))
		{
			OutError = FString::Printf(TEXT("NPY data is truncated in %s."), *Name);
			return false;
		}
		OutView.Data = Bytes.GetData() + DataOffset;
		OutView.ElementCount = ElementCount;
		return true;
	}

	double ReadNumeric(const FNpyArrayView& Array, const int64 Index)
	{
		const uint8* Source = Array.Data + Index * Array.ElementSize;
		switch (Array.ValueType)
		{
		case ENpyValueType::Float16:
		{
			FFloat16 Value;
			FMemory::Memcpy(&Value.Encoded, Source, sizeof(Value.Encoded));
			return Value.GetFloat();
		}
		case ENpyValueType::Float32:
		{
			float Value = 0.0f;
			FMemory::Memcpy(&Value, Source, sizeof(Value));
			return Value;
		}
		case ENpyValueType::Float64:
		{
			double Value = 0.0;
			FMemory::Memcpy(&Value, Source, sizeof(Value));
			return Value;
		}
		case ENpyValueType::Int32:
		{
			int32 Value = 0;
			FMemory::Memcpy(&Value, Source, sizeof(Value));
			return Value;
		}
		case ENpyValueType::Int64:
		{
			int64 Value = 0;
			FMemory::Memcpy(&Value, Source, sizeof(Value));
			return static_cast<double>(Value);
		}
		default:
			return 0.0;
		}
	}

	bool IsFloatType(const ENpyValueType Type)
	{
		return Type == ENpyValueType::Float16
			|| Type == ENpyValueType::Float32
			|| Type == ENpyValueType::Float64;
	}
}

bool FRoutePreviewNpzLoader::Load(
	const FString& FilePath,
	FRoutePreviewData& OutData,
	FString& OutError)
{
	using namespace DroneGCSRoutePreviewNpzPrivate;
	OutData = FRoutePreviewData{};
	OutError.Reset();

	TArray<uint8> Archive;
	if (!FFileHelper::LoadFileToArray(Archive, *FilePath))
	{
		OutError = FString::Printf(TEXT("Unable to read NPZ file: %s"), *FilePath);
		return false;
	}

	// Hash the exact in-memory archive that will be parsed below. Never reopen
	// FilePath after parsing: SourceDatasetDigest must be immune to TOCTOU.
	FRouteArtifactDigest256 SourceDatasetDigest;
	if (!FRouteArtifactSha256::HashBytes(Archive, SourceDatasetDigest, OutError)
		|| !FRouteArtifactSourceIdentity::Create(
			SourceDatasetDigest,
			Archive.Num(),
			FilePath,
			OutData.SourceIdentity,
			OutError))
	{
		OutData = FRoutePreviewData{};
		return false;
	}

	TArray<FZipEntry> Entries;
	if (!ReadZipDirectory(Archive, Entries, OutError))
	{
		return false;
	}

	const FZipEntry* PositionEntry = nullptr;
	const FZipEntry* ColorEntry = nullptr;
	const FZipEntry* RangeEntry = nullptr;
	for (const FZipEntry& Entry : Entries)
	{
		const FString CleanName = FPaths::GetCleanFilename(Entry.Name);
		if (CleanName.Equals(TEXT("position.npy"), ESearchCase::IgnoreCase))
		{
			PositionEntry = &Entry;
		}
		else if (CleanName.Equals(TEXT("color.npy"), ESearchCase::IgnoreCase))
		{
			ColorEntry = &Entry;
		}
		else if (CleanName.Equals(TEXT("range.npy"), ESearchCase::IgnoreCase))
		{
			RangeEntry = &Entry;
		}
	}
	if (!PositionEntry)
	{
		OutError = TEXT("NPZ is missing required position.npy.");
		return false;
	}
	if (!ColorEntry)
	{
		OutError = TEXT("NPZ is missing required color.npy.");
		return false;
	}

	TArray<uint8> PositionBytes;
	TArray<uint8> ColorBytes;
	TArray<uint8> RangeBytes;
	if (!ExtractZipEntry(Archive, *PositionEntry, PositionBytes, OutError)
		|| !ExtractZipEntry(Archive, *ColorEntry, ColorBytes, OutError)
		|| (RangeEntry && !ExtractZipEntry(Archive, *RangeEntry, RangeBytes, OutError)))
	{
		return false;
	}
	Archive.Reset();

	FNpyArrayView PositionArray;
	FNpyArrayView ColorArray;
	FNpyArrayView RangeArray;
	if (!ParseNpy(TEXT("position.npy"), PositionBytes, PositionArray, OutError)
		|| !ParseNpy(TEXT("color.npy"), ColorBytes, ColorArray, OutError)
		|| (RangeEntry && !ParseNpy(TEXT("range.npy"), RangeBytes, RangeArray, OutError)))
	{
		return false;
	}
	if (!IsFloatType(PositionArray.ValueType) || PositionArray.Shape.Num() != 2)
	{
		OutError = TEXT("position.npy must be a rank-2 floating-point array.");
		return false;
	}
	if (!IsFloatType(ColorArray.ValueType) || ColorArray.Shape.Num() != 2)
	{
		OutError = TEXT("color.npy must be a rank-2 floating-point array.");
		return false;
	}
	if ((PositionArray.Shape[1] % 3) != 0)
	{
		OutError = TEXT("position.npy column count must be divisible by 3.");
		return false;
	}
	if ((ColorArray.Shape[1] % 4) != 0)
	{
		OutError = TEXT("color.npy column count must be divisible by 4.");
		return false;
	}

	const int64 FrameCount64 = PositionArray.Shape[0];
	const int64 PositionDroneCount = PositionArray.Shape[1] / 3;
	const int64 ColorDroneCount = ColorArray.Shape[1] / 4;
	if (ColorArray.Shape[0] != FrameCount64)
	{
		OutError = TEXT("position.npy and color.npy frame counts do not match.");
		return false;
	}
	if (PositionDroneCount != ColorDroneCount)
	{
		OutError = TEXT("position.npy and color.npy drone counts do not match.");
		return false;
	}
	if (FrameCount64 > MAX_int32 || PositionDroneCount > MAX_int32
		|| FrameCount64 * PositionDroneCount > MAX_int32)
	{
		OutError = TEXT("Route sample count exceeds UE TArray limits.");
		return false;
	}

	OutData.FrameCount = static_cast<int32>(FrameCount64);
	OutData.DroneCount = static_cast<int32>(PositionDroneCount);
	const int32 SampleCount = OutData.FrameCount * OutData.DroneCount;
	OutData.PositionsENU.SetNumUninitialized(SampleCount);
	OutData.Colors.SetNumUninitialized(SampleCount);

	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const int64 PositionBase = static_cast<int64>(SampleIndex) * 3;
		const double X = ReadNumeric(PositionArray, PositionBase);
		const double Y = ReadNumeric(PositionArray, PositionBase + 1);
		const double Z = ReadNumeric(PositionArray, PositionBase + 2);
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
		{
			OutError = FString::Printf(TEXT("Non-finite position value at route sample %d."), SampleIndex);
			OutData = FRoutePreviewData{};
			return false;
		}
		const FVector3f Position(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
		if (Position.ContainsNaN())
		{
			OutError = FString::Printf(TEXT("Position overflows float storage at route sample %d."), SampleIndex);
			OutData = FRoutePreviewData{};
			return false;
		}
		OutData.PositionsENU[SampleIndex] = Position;

		const int64 ColorBase = static_cast<int64>(SampleIndex) * 4;
		const double R = ReadNumeric(ColorArray, ColorBase);
		const double G = ReadNumeric(ColorArray, ColorBase + 1);
		const double B = ReadNumeric(ColorArray, ColorBase + 2);
		const double SourceAlpha = ReadNumeric(ColorArray, ColorBase + 3);
		if (!FMath::IsFinite(R) || !FMath::IsFinite(G)
			|| !FMath::IsFinite(B) || !FMath::IsFinite(SourceAlpha))
		{
			OutError = FString::Printf(TEXT("Non-finite color value at route sample %d."), SampleIndex);
			OutData = FRoutePreviewData{};
			return false;
		}
		OutData.Colors[SampleIndex] = FFloat16Color(FLinearColor(
			static_cast<float>(R),
			static_cast<float>(G),
			static_cast<float>(B),
			1.0f));
	}

	if (RangeEntry)
	{
		if (RangeArray.Shape.Num() != 1 || RangeArray.Shape[0] != 2
			|| (RangeArray.ValueType != ENpyValueType::Int32
				&& RangeArray.ValueType != ENpyValueType::Int64))
		{
			OutError = TEXT("range.npy must be a two-element int32/int64 vector.");
			OutData = FRoutePreviewData{};
			return false;
		}
		const int64 RangeStart = static_cast<int64>(ReadNumeric(RangeArray, 0));
		const int64 RangeEnd = static_cast<int64>(ReadNumeric(RangeArray, 1));
		if (RangeStart < 0 || RangeEnd < RangeStart
			|| RangeStart >= OutData.FrameCount || RangeEnd >= OutData.FrameCount)
		{
			OutError = FString::Printf(
				TEXT("range.npy [%lld,%lld] is outside frame range [0,%d]."),
				RangeStart,
				RangeEnd,
				OutData.FrameCount - 1);
			OutData = FRoutePreviewData{};
			return false;
		}
		OutData.OptionalRangeStart = static_cast<int32>(RangeStart);
		OutData.OptionalRangeEnd = static_cast<int32>(RangeEnd);
	}

	return OutData.IsLoaded();
}
