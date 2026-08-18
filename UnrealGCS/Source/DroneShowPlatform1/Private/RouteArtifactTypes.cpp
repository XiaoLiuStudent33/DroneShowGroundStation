#include "RouteArtifactTypes.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "RouteArtifactSharedAdapter.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <bcrypt.h>
#endif

namespace
{
	bool ContainsNul(const FString& Value)
	{
		for (int32 CharacterIndex = 0; CharacterIndex < Value.Len(); ++CharacterIndex)
		{
			const TCHAR Character = Value[CharacterIndex];
			if (Character == TEXT('\0'))
			{
				return true;
			}
		}
		return false;
	}

	bool ValidateCanonicalString(
		const FString& Value,
		const TCHAR* FieldName,
		FString& OutError)
	{
		if (Value.IsEmpty() || ContainsNul(Value))
		{
			OutError = FString::Printf(TEXT("%s must be non-empty and contain no NUL."), FieldName);
			return false;
		}
		FTCHARToUTF8 Utf8(*Value);
		if (Utf8.Length() <= 0)
		{
			OutError = FString::Printf(TEXT("%s has no canonical UTF-8 bytes."), FieldName);
			return false;
		}
		return true;
	}

}

void FRouteArtifactDigest256::Reset()
{
	Bytes.fill(0);
	bIsSet = false;
}

FString FRouteArtifactDigest256::ToLowerHex() const
{
	if (!bIsSet)
	{
		return FString{};
	}
	static constexpr TCHAR HexDigits[] = TEXT("0123456789abcdef");
	FString Result;
	Result.Reserve(LowerHexCharacterCount);
	for (const uint8 Byte : Bytes)
	{
		Result.AppendChar(HexDigits[(Byte >> 4) & 0x0f]);
		Result.AppendChar(HexDigits[Byte & 0x0f]);
	}
	return Result;
}

bool FRouteArtifactDigest256::ParseLowerHex(
	const FString& LowerHex,
	FRouteArtifactDigest256& OutDigest,
	FString& OutError)
{
	OutDigest.Reset();
	OutError.Reset();
	if (LowerHex.Len() != LowerHexCharacterCount)
	{
		OutError = TEXT("A SHA-256 digest must contain exactly 64 lowercase hex characters.");
		return false;
	}

	auto HexValue = [](const TCHAR Character) -> int32
	{
		if (Character >= TEXT('0') && Character <= TEXT('9'))
		{
			return Character - TEXT('0');
		}
		if (Character >= TEXT('a') && Character <= TEXT('f'))
		{
			return Character - TEXT('a') + 10;
		}
		return INDEX_NONE;
	};

	for (int32 ByteIndex = 0; ByteIndex < ByteCount; ++ByteIndex)
	{
		const int32 High = HexValue(LowerHex[ByteIndex * 2]);
		const int32 Low = HexValue(LowerHex[ByteIndex * 2 + 1]);
		if (High == INDEX_NONE || Low == INDEX_NONE)
		{
			OutError = TEXT("A SHA-256 digest may contain only lowercase 0-9 and a-f.");
			return false;
		}
		OutDigest.Bytes[ByteIndex] = static_cast<uint8>((High << 4) | Low);
	}
	OutDigest.bIsSet = true;
	return true;
}

bool FRouteArtifactDigest256::FromBytes(
	const uint8* Data,
	const int32 DataBytes,
	FRouteArtifactDigest256& OutDigest,
	FString& OutError)
{
	OutDigest.Reset();
	OutError.Reset();
	if (!Data || DataBytes != ByteCount)
	{
		OutError = TEXT("A SHA-256 digest requires exactly 32 bytes.");
		return false;
	}
	FMemory::Memcpy(OutDigest.Bytes.data(), Data, ByteCount);
	OutDigest.bIsSet = true;
	return true;
}

struct FRouteArtifactSha256Builder::FImpl
{
#if PLATFORM_WINDOWS
	BCRYPT_ALG_HANDLE Algorithm = nullptr;
	BCRYPT_HASH_HANDLE Hash = nullptr;
	TArray<uint8> HashObject;
#endif
	bool bInitialized = false;
	bool bFinalized = false;

	~FImpl()
	{
#if PLATFORM_WINDOWS
		if (Hash)
		{
			BCryptDestroyHash(Hash);
		}
		if (Algorithm)
		{
			BCryptCloseAlgorithmProvider(Algorithm, 0);
		}
#endif
	}
};

FRouteArtifactSha256Builder::FRouteArtifactSha256Builder() = default;
FRouteArtifactSha256Builder::~FRouteArtifactSha256Builder() = default;

bool FRouteArtifactSha256Builder::Initialize(FString& OutError)
{
	OutError.Reset();
	Impl = MakeUnique<FImpl>();
#if PLATFORM_WINDOWS
	NTSTATUS Status = BCryptOpenAlgorithmProvider(
		&Impl->Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	DWORD HashObjectBytes = 0;
	DWORD HashBytes = 0;
	DWORD ReturnedBytes = 0;
	if (Status >= 0)
	{
		Status = BCryptGetProperty(
			Impl->Algorithm,
			BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&HashObjectBytes),
			sizeof(HashObjectBytes),
			&ReturnedBytes,
			0);
	}
	if (Status >= 0)
	{
		Status = BCryptGetProperty(
			Impl->Algorithm,
			BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&HashBytes),
			sizeof(HashBytes),
			&ReturnedBytes,
			0);
	}
	if (Status >= 0 && HashBytes != FRouteArtifactDigest256::ByteCount)
	{
		Status = static_cast<NTSTATUS>(-1);
	}
	if (Status >= 0)
	{
		Impl->HashObject.SetNumUninitialized(static_cast<int32>(HashObjectBytes));
		Status = BCryptCreateHash(
			Impl->Algorithm,
			&Impl->Hash,
			Impl->HashObject.GetData(),
			HashObjectBytes,
			nullptr,
			0,
			0);
	}
	if (Status < 0)
	{
		Impl.Reset();
		OutError = TEXT("Windows CNG could not initialize an Artifact SHA-256 hash.");
		return false;
	}
	Impl->bInitialized = true;
	return true;
#else
	Impl.Reset();
	OutError = TEXT("No production Artifact SHA-256 provider is configured for this platform.");
	return false;
#endif
}

bool FRouteArtifactSha256Builder::Update(
	const void* Data,
	const uint64 ByteCount,
	FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bInitialized || Impl->bFinalized)
	{
		OutError = TEXT("Artifact SHA-256 builder is not active.");
		return false;
	}
	if (ByteCount > 0 && !Data)
	{
		OutError = TEXT("Artifact SHA-256 input pointer is null for non-empty data.");
		return false;
	}
#if PLATFORM_WINDOWS
	const uint8* Cursor = static_cast<const uint8*>(Data);
	uint64 Remaining = ByteCount;
	while (Remaining > 0)
	{
		const ULONG ChunkBytes = static_cast<ULONG>(
			FMath::Min<uint64>(Remaining, MAX_uint32));
		const NTSTATUS Status = BCryptHashData(
			Impl->Hash,
			const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(Cursor)),
			ChunkBytes,
			0);
		if (Status < 0)
		{
			OutError = TEXT("Windows CNG failed while updating an Artifact SHA-256 hash.");
			return false;
		}
		Cursor += ChunkBytes;
		Remaining -= ChunkBytes;
	}
	return true;
#else
	return false;
#endif
}

bool FRouteArtifactSha256Builder::Finalize(
	FRouteArtifactDigest256& OutDigest,
	FString& OutError)
{
	OutDigest.Reset();
	OutError.Reset();
	if (!Impl || !Impl->bInitialized || Impl->bFinalized)
	{
		OutError = TEXT("Artifact SHA-256 builder cannot be finalized in its current state.");
		return false;
	}
#if PLATFORM_WINDOWS
	uint8 Signature[FRouteArtifactDigest256::ByteCount]{};
	const NTSTATUS Status = BCryptFinishHash(
		Impl->Hash,
		Signature,
		FRouteArtifactDigest256::ByteCount,
		0);
	Impl->bFinalized = true;
	if (Status < 0)
	{
		OutError = TEXT("Windows CNG failed while finalizing an Artifact SHA-256 hash.");
		return false;
	}
	return FRouteArtifactDigest256::FromBytes(
		Signature, FRouteArtifactDigest256::ByteCount, OutDigest, OutError);
#else
	return false;
#endif
}

bool FRouteArtifactSha256::HashBytes(
	const TArray<uint8>& Bytes,
	FRouteArtifactDigest256& OutDigest,
	FString& OutError)
{
	return HashBytes(Bytes.GetData(), static_cast<uint64>(Bytes.Num()), OutDigest, OutError);
}

bool FRouteArtifactSha256::HashBytes(
	const void* Data,
	const uint64 ByteCount,
	FRouteArtifactDigest256& OutDigest,
	FString& OutError)
{
	FRouteArtifactSha256Builder Builder;
	return Builder.Initialize(OutError)
		&& Builder.Update(Data, ByteCount, OutError)
		&& Builder.Finalize(OutDigest, OutError);
}

bool FRouteArtifactSha256::HashFileStreaming(
	const FString& FilePath,
	FRouteArtifactDigest256& OutDigest,
	int64& OutFileByteSize,
	FString& OutError)
{
	OutDigest.Reset();
	OutFileByteSize = 0;
	OutError.Reset();
	TUniquePtr<IFileHandle> FileHandle(
		FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FilePath));
	if (!FileHandle)
	{
		OutError = FString::Printf(TEXT("Unable to open Artifact file: %s"), *FilePath);
		return false;
	}

	const int64 SizeAtOpen = FileHandle->Size();
	if (SizeAtOpen < 0)
	{
		OutError = TEXT("Artifact file handle reported an invalid size.");
		return false;
	}

	FRouteArtifactSha256Builder Builder;
	if (!Builder.Initialize(OutError))
	{
		return false;
	}
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(StreamingBufferBytes);
	while (OutFileByteSize < SizeAtOpen)
	{
		const int64 Remaining = SizeAtOpen - OutFileByteSize;
		const int64 ReadBytes = FMath::Min<int64>(Remaining, Buffer.Num());
		if (!FileHandle->Read(Buffer.GetData(), ReadBytes))
		{
			OutDigest.Reset();
			OutFileByteSize = 0;
			OutError = TEXT("Artifact file read failed before all opened bytes were hashed.");
			return false;
		}
		if (!Builder.Update(Buffer.GetData(), static_cast<uint64>(ReadBytes), OutError))
		{
			OutDigest.Reset();
			OutFileByteSize = 0;
			return false;
		}
		OutFileByteSize += ReadBytes;
	}

	// Size and hash are derived from the same open handle. A concurrent size
	// change invalidates the result rather than producing a mixed identity.
	if (FileHandle->Size() != SizeAtOpen || FileHandle->Tell() != SizeAtOpen)
	{
		OutDigest.Reset();
		OutFileByteSize = 0;
		OutError = TEXT("Artifact file changed size while it was being hashed.");
		return false;
	}
	if (!Builder.Finalize(OutDigest, OutError))
	{
		OutFileByteSize = 0;
		return false;
	}
	return true;
}

bool FRouteArtifactAssignmentBinding::Validate(FString& OutError) const
{
	OutError.Reset();
	if (!ValidateCanonicalString(FleetSessionID, TEXT("FleetSessionID"), OutError)
		|| !ValidateCanonicalString(
			AssignmentAuthoritySessionID, TEXT("AssignmentAuthoritySessionID"), OutError))
	{
		return false;
	}
	if (AssignmentRevision <= 0)
	{
		OutError = TEXT("AssignmentRevision must be greater than zero.");
		return false;
	}
	if (!AssignmentDigest.IsValid())
	{
		OutError = TEXT("AssignmentDigest is not a valid fixed SHA-256 value.");
		return false;
	}
	return true;
}

bool FRouteArtifactSourceIdentity::IsValid() const
{
	if (!SourceDatasetDigest.IsValid() || SourceByteSize <= 0
		|| SourceDisplayFileName.IsEmpty() || ContainsNul(SourceDisplayFileName)
		|| SourceDisplayFileName == TEXT(".") || SourceDisplayFileName == TEXT(".."))
	{
		return false;
	}
	return !SourceDisplayFileName.Contains(TEXT("/"))
		&& !SourceDisplayFileName.Contains(TEXT("\\"))
		&& FPaths::GetCleanFilename(SourceDisplayFileName) == SourceDisplayFileName;
}

void FRouteArtifactSourceIdentity::Reset()
{
	SourceDatasetDigest.Reset();
	SourceByteSize = 0;
	SourceDisplayFileName.Reset();
}

bool FRouteArtifactSourceIdentity::Create(
	const FRouteArtifactDigest256& Digest,
	const int64 ByteSize,
	const FString& SourcePath,
	FRouteArtifactSourceIdentity& OutIdentity,
	FString& OutError)
{
	OutIdentity.Reset();
	OutError.Reset();
	FString NormalizedPath = SourcePath;
	FPaths::NormalizeFilename(NormalizedPath);
	OutIdentity.SourceDatasetDigest = Digest;
	OutIdentity.SourceByteSize = ByteSize;
	OutIdentity.SourceDisplayFileName = FPaths::GetCleanFilename(NormalizedPath);
	if (!OutIdentity.IsValid())
	{
		OutIdentity.Reset();
		OutError = TEXT("Route source identity requires a digest, positive byte size, and safe basename.");
		return false;
	}
	return true;
}

bool FRouteArtifactBuildIdentity::Validate(FString& OutError) const
{
	OutError.Reset();
	if (ArtifactSchemaVersion == 0)
	{
		OutError = TEXT("ArtifactSchemaVersion must be greater than zero.");
		return false;
	}
	if (!AssignmentBinding.Validate(OutError))
	{
		return false;
	}
	if (!SourceDatasetDigest.IsValid())
	{
		OutError = TEXT("SourceDatasetDigest is not a valid fixed SHA-256 value.");
		return false;
	}
	if (!ValidateCanonicalString(
		WaypointGeneratorVersion, TEXT("WaypointGeneratorVersion"), OutError))
	{
		return false;
	}
	if (!GeneratorInputDigest.IsValid())
	{
		OutError = TEXT("GeneratorInputDigest is not a valid fixed SHA-256 value.");
		return false;
	}
	return true;
}

bool RouteArtifactIdentity::SerializeCanonicalBuildIdentity(
	const FRouteArtifactBuildIdentity& Identity,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	OutBytes.Reset();
	OutError.Reset();
	std::vector<uint8> SharedBytes;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::SerializeCanonicalBuildIdentity(
		RouteArtifactSharedAdapter::ToShared(Identity), SharedBytes, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutBytes.Append(SharedBytes.data(), static_cast<int32>(SharedBytes.size()));
	return true;
}

bool RouteArtifactIdentity::ComputeArtifactSetID(
	const FRouteArtifactBuildIdentity& Identity,
	FString& OutArtifactSetID,
	FString& OutError)
{
	OutArtifactSetID.Reset();
	dronegcs::route_artifact::Error SharedError;
	std::string SharedSetID;
	if (!dronegcs::route_artifact::ComputeArtifactSetId(
		RouteArtifactSharedAdapter::ToShared(Identity), SharedSetID, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutArtifactSetID = RouteArtifactSharedAdapter::FromUtf8(SharedSetID);
	return true;
}
