#include "RouteArtifactWorkspaceManager.h"

#include "RouteArtifactReadLease.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactFilesystemSafety.h"
#include "RouteArtifactSharedAdapter.h"
#include "RouteArtifactWorkspacePaths.h"

DEFINE_LOG_CATEGORY_STATIC(LogRouteArtifactWorkspace, Log, All);

namespace
{
	FCriticalSection GWorkspaceWriterRegistryMutex;
	TSet<FString> GWorkspaceWriterRoots;

	FString CurrentPath(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT("Current"));
	}

	FString StagingPath(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT("Staging"));
	}

	bool WorkspaceSameBinding(
		const FRouteArtifactAssignmentBinding& Left,
		const FRouteArtifactAssignmentBinding& Right)
	{
		return Left.FleetSessionID == Right.FleetSessionID
			&& Left.AssignmentAuthoritySessionID == Right.AssignmentAuthoritySessionID
			&& Left.AssignmentRevision == Right.AssignmentRevision
			&& Left.AssignmentDigest == Right.AssignmentDigest;
	}

	bool SameSource(
		const FRouteArtifactSourceIdentity& Left,
		const FRouteArtifactSourceIdentity& Right)
	{
		return Left.SourceDatasetDigest == Right.SourceDatasetDigest
			&& Left.SourceByteSize == Right.SourceByteSize
			&& Left.SourceDisplayFileName == Right.SourceDisplayFileName;
	}

	bool ValidateGate(
		const FRouteArtifactBuildIdentity& BuildIdentity,
		const FRouteArtifactWorkspaceAuthorityGate& Gate,
		FString& OutError)
	{
		if (!BuildIdentity.Validate(OutError))
		{
			return false;
		}
		if (!Gate.bHasFrozenSnapshot)
		{
			OutError = TEXT("FrozenAssignmentRequired: no frozen Route Assignment is available.");
			return false;
		}
		if (!Gate.bConsoleAccepted)
		{
			OutError = TEXT("ConsoleAcceptedRequired: Frozen Assignment has not been accepted.");
			return false;
		}
		if (!Gate.ConsoleAcceptedBinding.Validate(OutError)
			|| !WorkspaceSameBinding(BuildIdentity.AssignmentBinding, Gate.ConsoleAcceptedBinding)
			|| Gate.FrozenFleetSessionID != BuildIdentity.AssignmentBinding.FleetSessionID
			|| Gate.FrozenAssignmentAuthoritySessionID
				!= BuildIdentity.AssignmentBinding.AssignmentAuthoritySessionID
			|| Gate.FrozenAssignmentRevision
				!= BuildIdentity.AssignmentBinding.AssignmentRevision)
		{
			OutError = TEXT("AssignmentGateMismatch: Frozen/ConsoleAccepted/Build identities differ.");
			return false;
		}
		if (!Gate.bSourceExists || !Gate.ActiveSourceIdentity.IsValid()
			|| Gate.RouteSlotCount <= 0
			|| Gate.ActiveSourceIdentity.SourceDatasetDigest
				!= BuildIdentity.SourceDatasetDigest)
		{
			OutError = TEXT("SourceGateMismatch: active Route source is absent, empty, or has another digest.");
			return false;
		}
		return true;
	}

	bool WriteFlushedFile(const FString& Path, const TArray<uint8>& Bytes, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Path, false, false));
		if (!Handle || (Bytes.Num() > 0 && !Handle->Write(Bytes.GetData(), Bytes.Num()))
			|| !Handle->Flush(true))
		{
			OutError = TEXT("Artifact workspace could not write and flush a required file.");
			return false;
		}
		Handle.Reset();
		return true;
	}

	bool WriteByTemporaryRename(
		const FString& Directory,
		const TCHAR* TemporaryName,
		const TCHAR* FinalName,
		const TArray<uint8>& Bytes,
		FString& OutError)
	{
		IFileManager& Files = IFileManager::Get();
		const FString TemporaryPath = FPaths::Combine(Directory, TemporaryName);
		const FString FinalPath = FPaths::Combine(Directory, FinalName);
		if (Files.FileExists(*TemporaryPath) || Files.FileExists(*FinalPath))
		{
			OutError = TEXT("Artifact workspace staging metadata already exists.");
			return false;
		}
		if (!WriteFlushedFile(TemporaryPath, Bytes, OutError))
		{
			return false;
		}
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.MoveFile(*FinalPath, *TemporaryPath))
		{
			OutError = TEXT("Artifact workspace atomic metadata promotion failed.");
			return false;
		}
		return true;
	}

	bool LoadBytes(const FString& Path, TArray<uint8>& OutBytes, FString& OutError)
	{
		OutBytes.Reset();
		if (!FFileHelper::LoadFileToArray(OutBytes, *Path))
		{
			OutError = TEXT("Artifact workspace could not read a required file.");
			return false;
		}
		return true;
	}

	bool MoveDirectory(const FString& Destination, const FString& Source)
	{
		// Platform MoveFile maps to the OS rename primitive and supports a directory
		// on the same volume. IFileManager::Move is file-oriented on Windows.
		return FPlatformFileManager::Get().GetPlatformFile().MoveFile(
			*Destination, *Source);
	}

	bool DeleteTree(const FString& Path)
	{
		FString IgnoredError;
		return RouteArtifactFilesystemSafety::DeleteTreeNoReparse(Path, IgnoredError);
	}

	bool IsLowerHexGuidDigits(const FString& Name)
	{
		if (Name.Len() != 32) return false;
		for (const TCHAR Character : Name)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	bool IsPreviousDirectoryName(const FString& Name)
	{
		static const FString Prefix(TEXT(".previous-"));
		return Name.StartsWith(Prefix, ESearchCase::CaseSensitive)
			&& IsLowerHexGuidDigits(Name.Mid(Prefix.Len()));
	}

	struct FStagingRecoveryEntries
	{
		TArray<FString> Attempts;
		TArray<FString> Previous;
		int32 UnknownCount = 0;
	};

	bool EnumerateStagingRecoveryEntries(
		const FString& WorkspaceRoot,
		const FString& Staging,
		FStagingRecoveryEntries& OutEntries,
		FString& OutError)
	{
		OutEntries = FStagingRecoveryEntries{};
		IFileManager& Files = IFileManager::Get();
		if (!Files.DirectoryExists(*Staging)) return true;
		if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			WorkspaceRoot, Staging, true, true, OutError))
		{
			return false;
		}
		bool bEntryValid = true;
		const bool bVisited = Files.IterateDirectory(
			*Staging,
			[&](const TCHAR* FilenameOrDirectory, const bool bIsDirectory)
			{
				const FString Entry(FilenameOrDirectory);
				FString EntryError;
				if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
					WorkspaceRoot, Entry, true, bIsDirectory, EntryError))
				{
					OutError = EntryError;
					bEntryValid = false;
					return false;
				}
				const FString Name = FPaths::GetCleanFilename(Entry);
				if (bIsDirectory && IsLowerHexGuidDigits(Name))
				{
					OutEntries.Attempts.Add(Entry);
				}
				else if (bIsDirectory && IsPreviousDirectoryName(Name))
				{
					OutEntries.Previous.Add(Entry);
				}
				else
				{
					++OutEntries.UnknownCount;
				}
				return true;
			});
		if (!bVisited || !bEntryValid)
		{
			if (OutError.IsEmpty()) OutError = TEXT("StagingEnumerationFailed.");
			return false;
		}
		return true;
	}

	bool MakeLockIdentity(
		const FString& NormalizedRoot,
		FString& OutRegistryKey,
		FString& OutLockName,
		FString& OutError)
	{
		FString CanonicalRoot = NormalizedRoot;
#if PLATFORM_WINDOWS
		CanonicalRoot.ToLowerInline();
#endif
		FTCHARToUTF8 Utf8(*CanonicalRoot);
		FRouteArtifactDigest256 Digest;
		if (!FRouteArtifactSha256::HashBytes(
			Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest, OutError))
		{
			return false;
		}
		OutRegistryKey = Digest.ToLowerHex();
		OutLockName = FString::Printf(TEXT("DroneGCS.RouteArtifactWriter.%s"), *OutRegistryKey);
		return true;
	}

	void AddDirectoryAndParents(const FString& Directory, TSet<FString>& InOutDirectories)
	{
		FString Current = Directory;
		while (!Current.IsEmpty() && Current != TEXT("."))
		{
			FPaths::NormalizeFilename(Current);
			InOutDirectories.Add(Current);
			const FString Parent = FPaths::GetPath(Current);
			if (Parent == Current) break;
			Current = Parent;
		}
	}

	bool ValidateCurrentEntryAllowlist(
		const FRouteArtifactManifest& Manifest,
		const TArray<FString>& RelativeFiles,
		const TArray<FString>& RelativeDirectories,
		FString& OutError)
	{
		TSet<FString> ExpectedFiles;
		ExpectedFiles.Reserve(Manifest.Routes.Num() + 2);
		ExpectedFiles.Add(RouteArtifactManifest::ManifestFilename);
		ExpectedFiles.Add(TEXT("READY"));
		TSet<FString> ExpectedDirectories;
		ExpectedDirectories.Reserve(Manifest.Routes.Num() / RouteArtifactWorkspacePaths::RouteSlotsPerShard + 1);
		for (const FRouteArtifactRecord& Route : Manifest.Routes)
		{
			ExpectedFiles.Add(Route.RelativePath);
			AddDirectoryAndParents(FPaths::GetPath(Route.RelativePath), ExpectedDirectories);
		}
		if (ExpectedFiles.Num() != RelativeFiles.Num()
			|| ExpectedDirectories.Num() != RelativeDirectories.Num())
		{
			OutError = TEXT("UnexpectedCurrentEntry.");
			return false;
		}
		for (const FString& File : RelativeFiles)
		{
			if (!ExpectedFiles.Contains(File))
			{
				OutError = TEXT("UnexpectedCurrentFile.");
				return false;
			}
		}
		for (const FString& Directory : RelativeDirectories)
		{
			if (!ExpectedDirectories.Contains(Directory))
			{
				OutError = TEXT("UnexpectedCurrentDirectory.");
				return false;
			}
		}
		return true;
	}

	bool ValidateArtifactFilesSafe(
		const FString& ArtifactRoot,
		const FRouteArtifactManifest& Manifest,
		FString& OutError)
	{
		if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			ArtifactRoot, ArtifactRoot, true, true, OutError))
		{
			return false;
		}
		for (const FRouteArtifactRecord& Route : Manifest.Routes)
		{
			FString RoutePath;
			if (!RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
				ArtifactRoot, Route.RelativePath, RoutePath, OutError)
				|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
					ArtifactRoot, RoutePath, true, false, OutError))
			{
				return false;
			}
			FRouteWaypointValidationResult Validation;
			if (!RouteWaypointValidator::ValidateAndHashFileStreaming(
					RoutePath,
					Route.RouteSlotIndex,
					Manifest.FrameCount,
					ERouteWaypointValidationLevel::WaypointSemantic,
					nullptr,
					Validation)
				|| Validation.FileByteSize != Route.FileByteSize
				|| Validation.FileDigest != Route.RouteArtifactDigest)
			{
				OutError = Validation.Diagnostic.IsEmpty()
					? TEXT("WaypointSemanticValidationOrDigestMismatch.")
					: Validation.Diagnostic;
				return false;
			}
		}
		return true;
	}

	bool ValidateArtifactFilePresenceSafe(
		const FString& ArtifactRoot,
		const FRouteArtifactManifest& Manifest,
		FString& OutError)
	{
		if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			ArtifactRoot, ArtifactRoot, true, true, OutError))
		{
			return false;
		}
		for (const FRouteArtifactRecord& Route : Manifest.Routes)
		{
			FString RoutePath;
			if (!RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
				ArtifactRoot, Route.RelativePath, RoutePath, OutError)
				|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
					ArtifactRoot, RoutePath, true, false, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool ValidateCurrentDirectory(
		const FString& Directory,
		FRouteArtifactManifest& OutManifest,
		FString& OutError,
		const bool bValidateRouteHashes = true)
	{
		IFileManager& Files = IFileManager::Get();
		if (!Files.DirectoryExists(*Directory))
		{
			OutError = TEXT("Current Artifact directory does not exist.");
			return false;
		}
		if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Directory, Directory, true, true, OutError))
		{
			return false;
		}
		const FString ManifestPath = FPaths::Combine(
			Directory, RouteArtifactManifest::ManifestFilename);
		const FString ReadyPath = FPaths::Combine(Directory, TEXT("READY"));
		if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Directory, ManifestPath, true, false, OutError)
			|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
				Directory, ReadyPath, true, false, OutError))
		{
			return false;
		}
		TArray<uint8> ManifestBytes;
		if (!LoadBytes(ManifestPath, ManifestBytes, OutError)
			|| !RouteArtifactManifest::ParseCanonicalManifest(
				ManifestBytes, OutManifest, OutError))
		{
			return false;
		}
		TArray<uint8> ReadyBytes;
		FString ReadySetID;
		FRouteArtifactDigest256 ReadyDigest;
		if (!LoadBytes(ReadyPath, ReadyBytes, OutError)
			|| !FRouteArtifactWorkspaceManager::ParseReadyMarker(
				ReadyBytes, ReadySetID, ReadyDigest, OutError)
			|| ReadySetID != OutManifest.ArtifactSetID
			|| ReadyDigest != OutManifest.ArtifactSetDigest)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("READY marker does not match the canonical Manifest.");
			}
			return false;
		}
		if (bValidateRouteHashes
			? !ValidateArtifactFilesSafe(Directory, OutManifest, OutError)
			: !ValidateArtifactFilePresenceSafe(Directory, OutManifest, OutError))
		{
			return false;
		}
		TArray<FString> RelativeFiles;
		TArray<FString> RelativeDirectories;
		return RouteArtifactFilesystemSafety::EnumerateTreeNoReparse(
			Directory, RelativeFiles, RelativeDirectories, OutError)
			&& ValidateCurrentEntryAllowlist(
				OutManifest, RelativeFiles, RelativeDirectories, OutError);
	}
}

struct FRouteArtifactWorkspaceManager::FImpl
{
	FString ConfiguredRoot;
	FString ActiveRoot;
	FString AttemptID;
	FString AttemptRoot;
	FString ArtifactSetID;
	FString RegistryKey;
	TUniquePtr<FSystemWideCriticalSection> SystemWriterGuard;
	FRouteArtifactExpectedInputs ExpectedInputs;
	int32 ExpectedRouteSlotCount = 0;
	ERouteArtifactCurrentStatus CurrentStatus = ERouteArtifactCurrentStatus::None;
	ERouteArtifactGenerationStatus GenerationStatus = ERouteArtifactGenerationStatus::None;
	FRouteArtifactCurrentInfo CurrentInfo;
	FRouteArtifactRecoveryResult LastRecoveryResult;
	bool bCurrentSwapInProgress = false;
	bool bRegistryOwned = false;
#if WITH_DEV_AUTOMATION_TESTS
	bool bTestFailAttemptPromotion = false;
	int64 TestFinalizeRouteValidationReadCount = 0;
#endif

	bool AcquireWriter(const FString& Root, FString& OutError)
	{
		FString NewRegistryKey;
		FString LockName;
		if (!MakeLockIdentity(Root, NewRegistryKey, LockName, OutError))
		{
			return false;
		}
		{
			FScopeLock Lock(&GWorkspaceWriterRegistryMutex);
			if (GWorkspaceWriterRoots.Contains(NewRegistryKey))
			{
				OutError = TEXT("WriterActive.");
				return false;
			}
			GWorkspaceWriterRoots.Add(NewRegistryKey);
			bRegistryOwned = true;
			RegistryKey = NewRegistryKey;
		}
		SystemWriterGuard = MakeUnique<FSystemWideCriticalSection>(
			LockName, FTimespan::Zero());
		if (!SystemWriterGuard->IsValid())
		{
			ReleaseWriter();
			OutError = TEXT("WriterActive.");
			return false;
		}
		return true;
	}

	void ReleaseWriter()
	{
		SystemWriterGuard.Reset();
		if (bRegistryOwned)
		{
			FScopeLock Lock(&GWorkspaceWriterRegistryMutex);
			GWorkspaceWriterRoots.Remove(RegistryKey);
		}
		bRegistryOwned = false;
		RegistryKey.Reset();
	}

	void ClearActive(const bool bFailed)
	{
		AttemptID.Reset();
		AttemptRoot.Reset();
		ArtifactSetID.Reset();
		ActiveRoot.Reset();
		ExpectedInputs = FRouteArtifactExpectedInputs{};
		ExpectedRouteSlotCount = 0;
		bCurrentSwapInProgress = false;
		ReleaseWriter();
		GenerationStatus = bFailed
			? ERouteArtifactGenerationStatus::Failed
			: ERouteArtifactGenerationStatus::None;
	}

	bool FailAndCleanup(const FString& SafeReason, FString& OutError)
	{
		const FString FailedAttempt = AttemptRoot;
		const FString FailedAttemptID = AttemptID;
		const FString FailedSetID = ArtifactSetID;
		const bool bDeleted = DeleteTree(FailedAttempt);
		ClearActive(true);
		OutError = SafeReason;
		if (!bDeleted)
		{
			OutError += TEXT(" Staging cleanup failed; recovery is required.");
		}
		UE_LOG(LogRouteArtifactWorkspace, Warning,
			TEXT("Artifact generation failed. Attempt=%s Set=%s"),
			*FailedAttemptID, *FailedSetID.Left(12));
		return false;
	}
};

FRouteArtifactWorkspaceManager::FRouteArtifactWorkspaceManager()
	: Impl(MakeUnique<FImpl>())
{
}

FRouteArtifactWorkspaceManager::~FRouteArtifactWorkspaceManager()
{
	Shutdown();
}

bool FRouteArtifactWorkspaceManager::SetConfiguredWorkspaceRoot(
	const FString& ConfiguredRoot,
	FString& OutError)
{
	if (HasActiveGeneration())
	{
		OutError = TEXT("RootChangeWhileGenerationActive: finalize or abort first.");
		return false;
	}
	FString Normalized;
	if (!RouteArtifactWorkspacePaths::ResolveConfiguredRoot(
		ConfiguredRoot, Normalized, OutError))
	{
		return false;
	}
	const FString PreviousConfiguredRoot = Impl->ConfiguredRoot;
	const ERouteArtifactCurrentStatus PreviousCurrentStatus = Impl->CurrentStatus;
	const FRouteArtifactCurrentInfo PreviousCurrentInfo = Impl->CurrentInfo;
	Impl->ConfiguredRoot = MoveTemp(Normalized);
	Impl->CurrentStatus = ERouteArtifactCurrentStatus::None;
	Impl->CurrentInfo = FRouteArtifactCurrentInfo{};
	FRouteArtifactRecoveryResult Recovery;
	if (!RecoverWorkspace(Recovery, OutError))
	{
		const FRouteArtifactRecoveryResult FailedRecovery = Impl->LastRecoveryResult;
		Impl->ConfiguredRoot = PreviousConfiguredRoot;
		Impl->CurrentStatus = PreviousCurrentStatus;
		Impl->CurrentInfo = PreviousCurrentInfo;
		Impl->LastRecoveryResult = FailedRecovery;
		return false;
	}
	return true;
}

bool FRouteArtifactWorkspaceManager::GetWorkspaceVolumeFreeBytes(
	int64& OutFreeBytes,
	FString& OutError) const
{
	OutFreeBytes = 0;
	FString Root;
	if (!ResolveWorkspaceRoot(Root, OutError))
	{
		return false;
	}
	FString Probe = Root;
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	while (!Probe.IsEmpty() && !PlatformFile.DirectoryExists(*Probe))
	{
		const FString Parent = FPaths::GetPath(Probe);
		if (Parent == Probe)
		{
			break;
		}
		Probe = Parent;
	}
	uint64 TotalBytes = 0;
	uint64 FreeBytes = 0;
	if (Probe.IsEmpty()
		|| !FPlatformMisc::GetDiskTotalAndFreeSpace(Probe, TotalBytes, FreeBytes))
	{
		OutError = TEXT("Workspace volume free-space query failed.");
		return false;
	}
	OutFreeBytes = FreeBytes > static_cast<uint64>(MAX_int64)
		? MAX_int64 : static_cast<int64>(FreeBytes);
	return true;
}

const FString& FRouteArtifactWorkspaceManager::GetConfiguredWorkspaceRoot() const
{
	return Impl->ConfiguredRoot;
}

bool FRouteArtifactWorkspaceManager::ResolveWorkspaceRoot(
	FString& OutRoot,
	FString& OutError) const
{
	if (!Impl->ConfiguredRoot.IsEmpty())
	{
		return RouteArtifactWorkspacePaths::ResolveConfiguredRoot(
			Impl->ConfiguredRoot, OutRoot, OutError);
	}
#if UE_BUILD_SHIPPING
	constexpr ERouteArtifactWorkspaceRootKind RootKind =
		ERouteArtifactWorkspaceRootKind::ShippingLocalUser;
#else
	constexpr ERouteArtifactWorkspaceRootKind RootKind =
		ERouteArtifactWorkspaceRootKind::Development;
#endif
	return RouteArtifactWorkspacePaths::ResolveDefaultRoot(RootKind, OutRoot, OutError);
}

ERouteArtifactCurrentStatus FRouteArtifactWorkspaceManager::GetCurrentStatus() const
{
	return Impl->CurrentStatus;
}

ERouteArtifactGenerationStatus FRouteArtifactWorkspaceManager::GetGenerationStatus() const
{
	return Impl->GenerationStatus;
}

bool FRouteArtifactWorkspaceManager::IsCurrentSwapInProgress() const
{
	return Impl->bCurrentSwapInProgress;
}

bool FRouteArtifactWorkspaceManager::HasActiveGeneration() const
{
	return !Impl->AttemptID.IsEmpty();
}

const FRouteArtifactCurrentInfo& FRouteArtifactWorkspaceManager::GetCurrentInfo() const
{
	return Impl->CurrentInfo;
}

const FRouteArtifactRecoveryResult&
FRouteArtifactWorkspaceManager::GetLastRecoveryResult() const
{
	return Impl->LastRecoveryResult;
}

bool FRouteArtifactWorkspaceManager::RecoverWorkspace(
	FRouteArtifactRecoveryResult& OutResult,
	FString& OutError)
{
	OutResult = FRouteArtifactRecoveryResult{};
	OutError.Reset();
	if (HasActiveGeneration())
	{
		OutResult.Status = ERouteArtifactRecoveryStatus::Busy;
		OutResult.ErrorCode = ERouteArtifactRecoveryErrorCode::WriterActive;
		OutResult.RecoverySummary = TEXT("Workspace writer is active.");
		Impl->LastRecoveryResult = OutResult;
		OutError = TEXT("WriterActive.");
		return false;
	}

	FString Root;
	if (!ResolveWorkspaceRoot(Root, OutError))
	{
		OutResult.Status = ERouteArtifactRecoveryStatus::Unrecoverable;
		OutResult.ErrorCode = ERouteArtifactRecoveryErrorCode::FilesystemOperationFailed;
		OutResult.RecoverySummary = TEXT("Workspace root resolution failed.");
		Impl->LastRecoveryResult = OutResult;
		return false;
	}
	if (!Impl->AcquireWriter(Root, OutError))
	{
		OutResult.Status = ERouteArtifactRecoveryStatus::Busy;
		OutResult.ErrorCode = ERouteArtifactRecoveryErrorCode::WriterActive;
		OutResult.RecoverySummary = TEXT("Workspace writer is active.");
		Impl->LastRecoveryResult = OutResult;
		return false;
	}

	auto Finish = [&](const ERouteArtifactRecoveryStatus Status,
		const ERouteArtifactRecoveryErrorCode ErrorCode,
		const TCHAR* Summary,
		const bool bSuccess)
	{
		OutResult.Status = Status;
		OutResult.ErrorCode = ErrorCode;
		OutResult.RecoverySummary = Summary;
		Impl->LastRecoveryResult = OutResult;
		Impl->ReleaseWriter();
		return bSuccess;
	};

	IFileManager& Files = IFileManager::Get();
	const bool bRootExists = Files.DirectoryExists(*Root);
	if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
		Root, Root, bRootExists, true, OutError))
	{
		return Finish(
			ERouteArtifactRecoveryStatus::Unrecoverable,
			OutError.Contains(TEXT("ReparsePointDetected"))
				? ERouteArtifactRecoveryErrorCode::ReparsePointDetected
				: ERouteArtifactRecoveryErrorCode::FilesystemOperationFailed,
			TEXT("Workspace root safety validation failed."), false);
	}
	if (!bRootExists)
	{
		Impl->CurrentStatus = ERouteArtifactCurrentStatus::None;
		Impl->CurrentInfo = FRouteArtifactCurrentInfo{};
		return Finish(ERouteArtifactRecoveryStatus::Stable,
			ERouteArtifactRecoveryErrorCode::None,
			TEXT("Workspace is stable and has no Current Artifact."), true);
	}

	const FString Current = CurrentPath(Root);
	const FString Staging = StagingPath(Root);
	FStagingRecoveryEntries Entries;
	if (!EnumerateStagingRecoveryEntries(Root, Staging, Entries, OutError))
	{
		return Finish(
			ERouteArtifactRecoveryStatus::Unrecoverable,
			OutError.Contains(TEXT("ReparsePointDetected"))
				? ERouteArtifactRecoveryErrorCode::ReparsePointDetected
				: ERouteArtifactRecoveryErrorCode::FilesystemOperationFailed,
			TEXT("Staging safety validation failed."), false);
	}

	FRouteArtifactManifest CurrentManifest;
	FString CurrentError;
	const bool bCurrentExists = Files.DirectoryExists(*Current);
	const bool bCurrentValid = bCurrentExists
		&& ValidateCurrentDirectory(Current, CurrentManifest, CurrentError);
	if (bCurrentExists && CurrentError.Contains(TEXT("ReparsePointDetected")))
	{
		Impl->CurrentStatus = ERouteArtifactCurrentStatus::Invalid;
		return Finish(
			ERouteArtifactRecoveryStatus::Unrecoverable,
			ERouteArtifactRecoveryErrorCode::ReparsePointDetected,
			TEXT("Current contains a reparse point."), false);
	}

	TArray<FString> ValidPrevious;
	for (const FString& Previous : Entries.Previous)
	{
		FRouteArtifactManifest PreviousManifest;
		FString PreviousError;
		if (ValidateCurrentDirectory(Previous, PreviousManifest, PreviousError))
		{
			ValidPrevious.Add(Previous);
		}
	}

	bool bCleanupFailure = false;
	auto CleanupPaths = [&](const TArray<FString>& Paths, int32& InOutCount)
	{
		for (const FString& Path : Paths)
		{
			if (!Files.DirectoryExists(*Path)) continue;
			FString DeleteError;
			if (RouteArtifactFilesystemSafety::DeleteTreeNoReparse(Path, DeleteError))
			{
				++InOutCount;
			}
			else
			{
				bCleanupFailure = true;
			}
		}
	};
	auto CleanupAttempts = [&]()
	{
		CleanupPaths(Entries.Attempts, OutResult.OrphanAttemptsCleaned);
	};
	auto ReloadCurrent = [&]()
	{
		FRouteArtifactManifest Reloaded;
		FString ReloadError;
		return LoadAndValidateCurrentArtifact(Reloaded, ReloadError);
	};
	auto CompleteWithCleanupState = [&](const bool bRecoveredPrevious)
	{
		if (Entries.UnknownCount > 0 || bCleanupFailure)
		{
			return Finish(
				ERouteArtifactRecoveryStatus::RecoveryRequired,
				Entries.UnknownCount > 0
					? ERouteArtifactRecoveryErrorCode::UnexpectedWorkspaceEntry
					: ERouteArtifactRecoveryErrorCode::FilesystemOperationFailed,
				TEXT("Workspace contains preserved entries requiring review."), true);
		}
		const bool bChanged = bRecoveredPrevious
			|| OutResult.OrphanAttemptsCleaned > 0
			|| OutResult.PreviousDirectoriesCleaned > 0;
		return Finish(
			bChanged ? ERouteArtifactRecoveryStatus::Recovered
				: ERouteArtifactRecoveryStatus::Stable,
			bRecoveredPrevious
				? ERouteArtifactRecoveryErrorCode::RecoveredPreviousCurrent
				: (bChanged ? ERouteArtifactRecoveryErrorCode::OrphanStagingCleaned
					: ERouteArtifactRecoveryErrorCode::None),
			bChanged ? TEXT("Workspace recovery completed.")
				: TEXT("Workspace is stable."), true);
	};

	if (bCurrentValid)
	{
		// A fully validated Current is always authoritative over every leftover.
		CleanupAttempts();
		CleanupPaths(Entries.Previous, OutResult.PreviousDirectoriesCleaned);
		if (!ReloadCurrent())
		{
			return Finish(ERouteArtifactRecoveryStatus::Unrecoverable,
				ERouteArtifactRecoveryErrorCode::FilesystemOperationFailed,
				TEXT("Current changed during recovery validation."), false);
		}
		return CompleteWithCleanupState(false);
	}

	if (!bCurrentExists)
	{
		if (ValidPrevious.Num() > 1)
		{
			CleanupAttempts();
			Impl->CurrentStatus = ERouteArtifactCurrentStatus::None;
			return Finish(ERouteArtifactRecoveryStatus::RecoveryRequired,
				ERouteArtifactRecoveryErrorCode::MultiplePreviousCandidates,
				TEXT("Multiple valid previous Current candidates require review."), true);
		}
		if (ValidPrevious.Num() == 1)
		{
			const FString Previous = ValidPrevious[0];
			if (!MoveDirectory(Current, Previous) || !ReloadCurrent())
			{
				if (Files.DirectoryExists(*Current) && !Files.DirectoryExists(*Previous))
				{
					MoveDirectory(Previous, Current);
				}
				return Finish(ERouteArtifactRecoveryStatus::Unrecoverable,
					ERouteArtifactRecoveryErrorCode::RollbackRequired,
					TEXT("Previous Current restoration failed."), false);
			}
			CleanupAttempts();
			CleanupPaths(Entries.Previous, OutResult.PreviousDirectoriesCleaned);
			return CompleteWithCleanupState(true);
		}

		CleanupAttempts();
		Impl->CurrentStatus = ERouteArtifactCurrentStatus::None;
		Impl->CurrentInfo = FRouteArtifactCurrentInfo{};
		if (Entries.Previous.Num() > 0)
		{
			return Finish(ERouteArtifactRecoveryStatus::RecoveryRequired,
				ERouteArtifactRecoveryErrorCode::InvalidCurrent,
				TEXT("No valid previous Current candidate exists."), true);
		}
		return CompleteWithCleanupState(false);
	}

	// Current exists but is invalid. Only one valid previous candidate is safe.
	Impl->CurrentStatus = ERouteArtifactCurrentStatus::Invalid;
	if (ValidPrevious.Num() != 1)
	{
		CleanupAttempts();
		return Finish(ERouteArtifactRecoveryStatus::RecoveryRequired,
			ValidPrevious.Num() > 1
				? ERouteArtifactRecoveryErrorCode::MultiplePreviousCandidates
				: ERouteArtifactRecoveryErrorCode::InvalidCurrent,
			ValidPrevious.Num() > 1
				? TEXT("Invalid Current has multiple valid previous candidates.")
				: TEXT("Invalid Current has no valid previous candidate."), true);
	}

	TArray<FString> CurrentTreeFiles;
	TArray<FString> CurrentTreeDirectories;
	FString TreeError;
	if (!RouteArtifactFilesystemSafety::EnumerateTreeNoReparse(
		Current, CurrentTreeFiles, CurrentTreeDirectories, TreeError))
	{
		return Finish(ERouteArtifactRecoveryStatus::Unrecoverable,
			ERouteArtifactRecoveryErrorCode::ReparsePointDetected,
			TEXT("Invalid Current contains an unsafe filesystem entry."), false);
	}
	if (!Files.MakeDirectory(*Staging, true))
	{
		return Finish(ERouteArtifactRecoveryStatus::Unrecoverable,
			ERouteArtifactRecoveryErrorCode::FilesystemOperationFailed,
			TEXT("Recovery staging directory creation failed."), false);
	}
	const FString InvalidHold = FPaths::Combine(
		Staging, FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
	const FString Previous = ValidPrevious[0];
	if (!MoveDirectory(InvalidHold, Current))
	{
		return Finish(ERouteArtifactRecoveryStatus::Unrecoverable,
			ERouteArtifactRecoveryErrorCode::FilesystemOperationFailed,
			TEXT("Invalid Current could not be isolated."), false);
	}
	if (!MoveDirectory(Current, Previous))
	{
		const bool bRolledBack = MoveDirectory(Current, InvalidHold);
		return Finish(
			bRolledBack ? ERouteArtifactRecoveryStatus::RecoveryRequired
				: ERouteArtifactRecoveryStatus::Unrecoverable,
			ERouteArtifactRecoveryErrorCode::RollbackRequired,
			TEXT("Previous Current move failed; rollback was attempted."), bRolledBack);
	}
	if (!ReloadCurrent())
	{
		const bool bMovedPreviousBack = MoveDirectory(Previous, Current);
		const bool bRestoredInvalid = bMovedPreviousBack
			&& MoveDirectory(Current, InvalidHold);
		return Finish(
			bRestoredInvalid ? ERouteArtifactRecoveryStatus::RecoveryRequired
				: ERouteArtifactRecoveryStatus::Unrecoverable,
			ERouteArtifactRecoveryErrorCode::RollbackRequired,
			TEXT("Restored Current failed validation; rollback was attempted."), bRestoredInvalid);
	}

	TArray<FString> CleanupPrevious = Entries.Previous;
	CleanupPaths(CleanupPrevious, OutResult.PreviousDirectoriesCleaned);
	TArray<FString> InvalidHoldArray{ InvalidHold };
	CleanupPaths(InvalidHoldArray, OutResult.OrphanAttemptsCleaned);
	CleanupAttempts();
	return CompleteWithCleanupState(true);
}

bool FRouteArtifactWorkspaceManager::BeginGeneration(
	const FRouteArtifactBuildIdentity& BuildIdentity,
	const FRouteArtifactWorkspaceAuthorityGate& Gate,
	FRouteArtifactGenerationHandle& OutHandle,
	FString& OutError)
{
	OutHandle = FRouteArtifactGenerationHandle{};
	OutError.Reset();
	if (HasActiveGeneration())
	{
		OutError = TEXT("GenerationAlreadyActive: this manager already owns an attempt.");
		return false;
	}
	if (Impl->LastRecoveryResult.Status == ERouteArtifactRecoveryStatus::RecoveryRequired
		|| Impl->LastRecoveryResult.Status == ERouteArtifactRecoveryStatus::Unrecoverable
		|| Impl->LastRecoveryResult.Status == ERouteArtifactRecoveryStatus::Busy)
	{
		OutError = TEXT("WorkspaceRecoveryRequired: recover the Workspace before generation.");
		return false;
	}
	if (!ValidateGate(BuildIdentity, Gate, OutError))
	{
		return false;
	}

	Impl->GenerationStatus = ERouteArtifactGenerationStatus::Creating;
	FString Root;
	FString ArtifactSetID;
	if (!ResolveWorkspaceRoot(Root, OutError)
		|| !RouteArtifactIdentity::ComputeArtifactSetID(
			BuildIdentity, ArtifactSetID, OutError))
	{
		Impl->GenerationStatus = ERouteArtifactGenerationStatus::Failed;
		return false;
	}

	if (!Impl->AcquireWriter(Root, OutError))
	{
		Impl->GenerationStatus = ERouteArtifactGenerationStatus::Failed;
		OutError = TEXT("GenerationAlreadyActive: Workspace root writer is active.");
		return false;
	}

	IFileManager& Files = IFileManager::Get();
	const FString Staging = StagingPath(Root);
	const FString AttemptID = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const FString AttemptRoot = FPaths::Combine(Staging, AttemptID);
	if (!Files.MakeDirectory(*Root, true)
		|| !Files.MakeDirectory(*Staging, true)
		|| !Files.MakeDirectory(*AttemptRoot, true)
		|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Root, Root, true, true, OutError)
		|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Root, Staging, true, true, OutError)
		|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Root, AttemptRoot, true, true, OutError))
	{
		Impl->ActiveRoot = Root;
		Impl->AttemptID = AttemptID;
		Impl->AttemptRoot = AttemptRoot;
		Impl->ArtifactSetID = ArtifactSetID;
		return Impl->FailAndCleanup(TEXT("WorkspaceDirectoryCreationFailed."), OutError);
	}

	Impl->ActiveRoot = Root;
	Impl->AttemptID = AttemptID;
	Impl->AttemptRoot = AttemptRoot;
	Impl->ArtifactSetID = ArtifactSetID;
	Impl->ExpectedInputs.BuildIdentity = BuildIdentity;
	Impl->ExpectedInputs.SourceIdentity = Gate.ActiveSourceIdentity;
	Impl->ExpectedRouteSlotCount = Gate.RouteSlotCount;
	Impl->GenerationStatus = ERouteArtifactGenerationStatus::Generating;
	OutHandle.AttemptID = AttemptID;
	OutHandle.ArtifactSetID = ArtifactSetID;
	OutHandle.StagingRoot = AttemptRoot;
	return true;
}

bool FRouteArtifactWorkspaceManager::ResolveSafeStagingRoutePath(
	const int32 RouteSlotIndex,
	FString& OutPath,
	FString& OutError) const
{
	OutPath.Reset();
	if (!HasActiveGeneration()
		|| Impl->GenerationStatus != ERouteArtifactGenerationStatus::Generating)
	{
		OutError = TEXT("GenerationNotWritable: no attempt is in Generating state.");
		return false;
	}
	if (RouteSlotIndex < 0 || RouteSlotIndex >= Impl->ExpectedRouteSlotCount)
	{
		OutError = TEXT("RouteSlotIndex is outside the active source domain.");
		return false;
	}
	FString RelativePath;
	if (!RouteArtifactWorkspacePaths::BuildRouteRelativePath(
		RouteSlotIndex, RelativePath, OutError)
		|| !RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
			Impl->AttemptRoot, RelativePath, OutPath, OutError)
		|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Impl->ActiveRoot, Impl->AttemptRoot, true, true, OutError))
	{
		return false;
	}
	const FString Parent = FPaths::GetPath(OutPath);
	if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
		Impl->ActiveRoot, Parent, false, true, OutError)
		|| !IFileManager::Get().MakeDirectory(*Parent, true)
		|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Impl->ActiveRoot, Parent, true, true, OutError)
		|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
			Impl->ActiveRoot, OutPath, false, false, OutError))
	{
		if (OutError.IsEmpty()) OutError = TEXT("SafeStagingRoutePathCreationFailed.");
		OutPath.Reset();
		return false;
	}
	return true;
}

bool FRouteArtifactWorkspaceManager::FinalizeGeneration(
	const FRouteArtifactManifest& Manifest,
	const FRouteArtifactWorkspaceAuthorityGate& CurrentGate,
	FString& OutError)
{
	return FinalizeGenerationInternal(Manifest, CurrentGate, false, OutError, nullptr);
}

bool FRouteArtifactWorkspaceManager::FinalizeGeneratedRoutes(
	FRouteArtifactManifest& InOutManifest,
	const FRouteArtifactWorkspaceAuthorityGate& CurrentGate,
	const FRouteWaypointSourceValidationContext& SourceValidationContext,
	FString& OutError,
	FRouteArtifactFinalizeMetrics* const OutMetrics,
	const TFunction<bool()>& IsCancelled)
{
	OutError.Reset();
	if (OutMetrics)
	{
		*OutMetrics = FRouteArtifactFinalizeMetrics{};
	}
	if (!HasActiveGeneration()
		|| Impl->GenerationStatus != ERouteArtifactGenerationStatus::Generating)
	{
		OutError = TEXT("GenerationNotActive: no writable attempt can be finalized.");
		return false;
	}
	if (!ValidateGate(Impl->ExpectedInputs.BuildIdentity, CurrentGate, OutError)
		|| !SameSource(CurrentGate.ActiveSourceIdentity, Impl->ExpectedInputs.SourceIdentity)
		|| CurrentGate.RouteSlotCount != Impl->ExpectedRouteSlotCount
		|| !SourceValidationContext.IsValid()
		|| !SameSource(SourceValidationContext.SourceIdentity, Impl->ExpectedInputs.SourceIdentity)
		|| SourceValidationContext.RouteCount != Impl->ExpectedRouteSlotCount
		|| SourceValidationContext.FrameCount != InOutManifest.FrameCount
		|| SourceValidationContext.RouteDatasetGeneration
			!= InOutManifest.RouteDatasetGeneration
		|| SourceValidationContext.GeneratorContext.GeneratorInputDigest
			!= InOutManifest.GeneratorInputDigest)
	{
		const FString Reason = OutError.IsEmpty()
			? TEXT("FinalizeAuthorityGateChanged.") : OutError;
		return Impl->FailAndCleanup(Reason, OutError);
	}
	if (InOutManifest.ArtifactSetID != Impl->ArtifactSetID
		|| InOutManifest.RouteSlotCount != Impl->ExpectedRouteSlotCount
		|| !InOutManifest.Routes.IsEmpty()
		|| InOutManifest.ArtifactSetDigest.IsValid())
	{
		return Impl->FailAndCleanup(
			TEXT("GeneratedRouteManifestDraftInvalid."), OutError);
	}

#if WITH_DEV_AUTOMATION_TESTS
	Impl->TestFinalizeRouteValidationReadCount = 0;
#endif
	InOutManifest.Routes.Reserve(Impl->ExpectedRouteSlotCount);
	const double ValidationStartSeconds = FPlatformTime::Seconds();
	for (int32 RouteSlotIndex = 0;
		RouteSlotIndex < Impl->ExpectedRouteSlotCount; ++RouteSlotIndex)
	{
		if (IsCancelled && IsCancelled())
		{
			return Impl->FailAndCleanup(
				TEXT("GeneratedRouteValidationCancelled."), OutError);
		}
		FRouteArtifactRecord& Record = InOutManifest.Routes.AddDefaulted_GetRef();
		Record.RouteSlotIndex = RouteSlotIndex;
		FString RoutePath;
		if (!RouteArtifactWorkspacePaths::BuildRouteRelativePath(
				RouteSlotIndex, Record.RelativePath, OutError)
			|| !RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
				Impl->AttemptRoot, Record.RelativePath, RoutePath, OutError)
			|| !RouteArtifactFilesystemSafety::ValidatePathNoReparse(
				Impl->AttemptRoot, RoutePath, true, false, OutError))
		{
			const FString Reason = OutError.IsEmpty()
				? TEXT("GeneratedRouteAuthoritativeHashFailed.") : OutError;
			return Impl->FailAndCleanup(Reason, OutError);
		}
		FRouteWaypointValidationResult Validation;
		if (!RouteWaypointValidator::ValidateAndHashFileStreaming(
				RoutePath,
				RouteSlotIndex,
				InOutManifest.FrameCount,
				ERouteWaypointValidationLevel::SourceBound,
				&SourceValidationContext,
				Validation,
				IsCancelled))
		{
			const FString Reason = Validation.Diagnostic.IsEmpty()
				? TEXT("GeneratedRouteSourceBoundValidationFailed.")
				: Validation.Diagnostic;
			return Impl->FailAndCleanup(Reason, OutError);
		}
		Record.RouteArtifactDigest = Validation.FileDigest;
		Record.FileByteSize = Validation.FileByteSize;
#if WITH_DEV_AUTOMATION_TESTS
		++Impl->TestFinalizeRouteValidationReadCount;
#endif
	}
	if (OutMetrics)
	{
		OutMetrics->AuthoritativeRouteValidationSeconds =
			FPlatformTime::Seconds() - ValidationStartSeconds;
	}
	const double DigestStartSeconds = FPlatformTime::Seconds();
	if (!RouteArtifactManifest::ComputeArtifactSetDigest(
		InOutManifest, InOutManifest.ArtifactSetDigest, OutError))
	{
		const FString Reason = OutError;
		return Impl->FailAndCleanup(Reason, OutError);
	}
	if (OutMetrics)
	{
		OutMetrics->ManifestConstructionSerializationSeconds =
			FPlatformTime::Seconds() - DigestStartSeconds;
	}
	return FinalizeGenerationInternal(
		InOutManifest, CurrentGate, true, OutError, OutMetrics);
}

bool FRouteArtifactWorkspaceManager::FinalizeGenerationInternal(
	const FRouteArtifactManifest& Manifest,
	const FRouteArtifactWorkspaceAuthorityGate& CurrentGate,
	const bool bRouteFilesAlreadyAuthoritativelyHashed,
	FString& OutError,
	FRouteArtifactFinalizeMetrics* const OutMetrics)
{
	OutError.Reset();
	if (!HasActiveGeneration()
		|| Impl->GenerationStatus != ERouteArtifactGenerationStatus::Generating)
	{
		OutError = TEXT("GenerationNotActive: no writable attempt can be finalized.");
		return false;
	}
	Impl->GenerationStatus = ERouteArtifactGenerationStatus::Validating;
	if (!ValidateGate(Impl->ExpectedInputs.BuildIdentity, CurrentGate, OutError)
		|| !SameSource(CurrentGate.ActiveSourceIdentity, Impl->ExpectedInputs.SourceIdentity)
		|| CurrentGate.RouteSlotCount != Impl->ExpectedRouteSlotCount)
	{
		const FString Reason = OutError.IsEmpty()
			? TEXT("FinalizeAuthorityGateChanged.") : OutError;
		return Impl->FailAndCleanup(Reason, OutError);
	}
	if (Manifest.ArtifactSetID != Impl->ArtifactSetID
		|| Manifest.RouteSlotCount != Impl->ExpectedRouteSlotCount
		|| !RouteArtifactManifest::ValidateManifestStructure(Manifest, OutError)
		|| !RouteArtifactManifest::ValidateManifestAgainstExpectedInputs(
			Manifest, Impl->ExpectedInputs, OutError)
		|| (bRouteFilesAlreadyAuthoritativelyHashed
			? !ValidateArtifactFilePresenceSafe(Impl->AttemptRoot, Manifest, OutError)
			: !ValidateArtifactFilesSafe(Impl->AttemptRoot, Manifest, OutError)))
	{
		const FString Reason = OutError.IsEmpty()
			? TEXT("FinalizeManifestOrRouteValidationFailed.") : OutError;
		return Impl->FailAndCleanup(Reason, OutError);
	}

	const double ManifestStartSeconds = FPlatformTime::Seconds();
	TArray<uint8> ManifestBytes;
	if (!RouteArtifactManifest::SerializeCanonicalManifest(
		Manifest, ManifestBytes, OutError)
		|| !WriteByTemporaryRename(
			Impl->AttemptRoot, TEXT("manifest.tmp"),
			RouteArtifactManifest::ManifestFilename, ManifestBytes, OutError))
	{
		const FString Reason = OutError;
		return Impl->FailAndCleanup(Reason, OutError);
	}
	TArray<uint8> PersistedManifestBytes;
	FRouteArtifactManifest PersistedManifest;
	if (!LoadBytes(FPaths::Combine(Impl->AttemptRoot,
			RouteArtifactManifest::ManifestFilename), PersistedManifestBytes, OutError)
		|| !RouteArtifactManifest::ParseCanonicalManifest(
			PersistedManifestBytes, PersistedManifest, OutError)
		|| !RouteArtifactManifest::ValidateManifestAgainstExpectedInputs(
			PersistedManifest, Impl->ExpectedInputs, OutError)
		|| (bRouteFilesAlreadyAuthoritativelyHashed
			? !ValidateArtifactFilePresenceSafe(Impl->AttemptRoot, PersistedManifest, OutError)
			: !ValidateArtifactFilesSafe(Impl->AttemptRoot, PersistedManifest, OutError)))
	{
		const FString Reason = OutError;
		return Impl->FailAndCleanup(Reason, OutError);
	}
	if (OutMetrics)
	{
		OutMetrics->ManifestConstructionSerializationSeconds +=
			FPlatformTime::Seconds() - ManifestStartSeconds;
	}

	const double PromotionStartSeconds = FPlatformTime::Seconds();
	TArray<uint8> ReadyBytes;
	if (!SerializeReadyMarker(
		Manifest.ArtifactSetID, Manifest.ArtifactSetDigest, ReadyBytes, OutError)
		|| !WriteByTemporaryRename(
			Impl->AttemptRoot, TEXT("READY.tmp"), TEXT("READY"), ReadyBytes, OutError))
	{
		const FString Reason = OutError;
		return Impl->FailAndCleanup(Reason, OutError);
	}

	dronegcs::route_artifact::ArtifactReadLease PromotionGate;
	bool bRecoveredAbandonedUploadLease = false;
	std::string PromotionGateError;
	const dronegcs::route_artifact::ArtifactReadLeaseAcquireResult GateResult =
		PromotionGate.TryAcquire(
			std::wstring(*Impl->ActiveRoot),
			bRecoveredAbandonedUploadLease,
			PromotionGateError);
	if (GateResult !=
		dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
	{
		Impl->GenerationStatus = ERouteArtifactGenerationStatus::Generating;
		OutError = GateResult ==
			dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Busy
			? TEXT("ArtifactCurrentLeaseBusy: an Upload Job is reading Current.")
			: FString::Printf(
				TEXT("ArtifactCurrentLeaseError: %s"),
				UTF8_TO_TCHAR(PromotionGateError.c_str()));
		return false;
	}
	if (bRecoveredAbandonedUploadLease)
	{
		UE_LOG(LogRouteArtifactWorkspace, Display,
			TEXT("Recovered abandoned Artifact Current lease before promotion."));
	}

	Impl->GenerationStatus = ERouteArtifactGenerationStatus::Finalizing;
	Impl->bCurrentSwapInProgress = true;
	IFileManager& Files = IFileManager::Get();
	const FString Current = CurrentPath(Impl->ActiveRoot);
	const FString Previous = FPaths::Combine(
		StagingPath(Impl->ActiveRoot), TEXT(".previous-") + Impl->AttemptID);
	const bool bHadCurrent = Files.DirectoryExists(*Current);
	if (bHadCurrent && !MoveDirectory(Previous, Current))
	{
		return Impl->FailAndCleanup(TEXT("CurrentToPreviousMoveFailed."), OutError);
	}

#if WITH_DEV_AUTOMATION_TESTS
	const bool bPromotionMoved = !Impl->bTestFailAttemptPromotion
		&& MoveDirectory(Current, Impl->AttemptRoot);
#else
	const bool bPromotionMoved = MoveDirectory(Current, Impl->AttemptRoot);
#endif
	if (!bPromotionMoved)
	{
		const bool bRolledBack = !bHadCurrent || MoveDirectory(Current, Previous);
		if (!bRolledBack)
		{
			Impl->ClearActive(true);
			Impl->CurrentStatus = ERouteArtifactCurrentStatus::Invalid;
			OutError = TEXT("AttemptPromotionAndRollbackFailed: recovery is required.");
			return false;
		}
		return Impl->FailAndCleanup(TEXT("AttemptPromotionFailed; previous Current restored."), OutError);
	}

	FRouteArtifactManifest PromotedManifest;
	FString PromotedError;
	if (!ValidateCurrentDirectory(
		Current, PromotedManifest, PromotedError,
		!bRouteFilesAlreadyAuthoritativelyHashed))
	{
		const bool bMovedBadCurrentBack = MoveDirectory(Impl->AttemptRoot, Current);
		const bool bRolledBack = !bHadCurrent || MoveDirectory(Current, Previous);
		if (!bMovedBadCurrentBack || !bRolledBack)
		{
			Impl->ClearActive(true);
			Impl->CurrentStatus = ERouteArtifactCurrentStatus::Invalid;
			OutError = TEXT("PromotedCurrentValidationAndRollbackFailed: recovery is required.");
			return false;
		}
		return Impl->FailAndCleanup(PromotedError, OutError);
	}

	if (bHadCurrent && !DeleteTree(Previous))
	{
		// New Current is already fully valid. Preserve the previous directory for 2C.3B.
		UE_LOG(LogRouteArtifactWorkspace, Warning,
			TEXT("Artifact replacement succeeded but previous cleanup requires recovery. Attempt=%s"),
			*Impl->AttemptID);
	}

	Impl->CurrentStatus = ERouteArtifactCurrentStatus::Ready;
	Impl->CurrentInfo.ArtifactSetID = PromotedManifest.ArtifactSetID;
	Impl->CurrentInfo.ArtifactSetDigest = PromotedManifest.ArtifactSetDigest;
	Impl->CurrentInfo.AssignmentBinding = PromotedManifest.AssignmentBinding;
	Impl->CurrentInfo.SourceDatasetDigest =
		PromotedManifest.SourceIdentity.SourceDatasetDigest;
	Impl->CurrentInfo.RouteSlotCount = PromotedManifest.RouteSlotCount;
	Impl->CurrentInfo.MatchedCount = PromotedManifest.MatchedCount;
	Impl->CurrentInfo.CurrentRoot = Current;
	Impl->CurrentInfo.CreatedAtUtc = PromotedManifest.CreatedAtUtc;
	Impl->ClearActive(false);
	if (OutMetrics)
	{
		OutMetrics->ReadyPromotionSeconds =
			FPlatformTime::Seconds() - PromotionStartSeconds;
	}
	return true;
}

bool FRouteArtifactWorkspaceManager::AbortGeneration(FString& OutError)
{
	OutError.Reset();
	if (!HasActiveGeneration())
	{
		OutError = TEXT("GenerationNotActive: repeated Abort is a safe failure.");
		return false;
	}
	const FString Attempt = Impl->AttemptRoot;
	const bool bDeleted = DeleteTree(Attempt);
	Impl->ClearActive(!bDeleted);
	if (!bDeleted)
	{
		OutError = TEXT("Abort cleanup failed; orphan staging is preserved for recovery.");
		return false;
	}
	return true;
}

bool FRouteArtifactWorkspaceManager::LoadAndValidateCurrentArtifact(
	FRouteArtifactManifest& OutManifest,
	FString& OutError)
{
	OutManifest = FRouteArtifactManifest{};
	FString Root;
	if (!ResolveWorkspaceRoot(Root, OutError))
	{
		Impl->CurrentStatus = ERouteArtifactCurrentStatus::Invalid;
		return false;
	}
	const FString Current = CurrentPath(Root);
	const bool bRootExists = IFileManager::Get().DirectoryExists(*Root);
	if (!RouteArtifactFilesystemSafety::ValidatePathNoReparse(
		Root, Root, bRootExists, true, OutError))
	{
		Impl->CurrentStatus = ERouteArtifactCurrentStatus::Invalid;
		Impl->CurrentInfo = FRouteArtifactCurrentInfo{};
		return false;
	}
	if (!IFileManager::Get().DirectoryExists(*Current))
	{
		Impl->CurrentStatus = ERouteArtifactCurrentStatus::None;
		Impl->CurrentInfo = FRouteArtifactCurrentInfo{};
		OutError = TEXT("Current Artifact is not present.");
		return false;
	}
	if (!ValidateCurrentDirectory(Current, OutManifest, OutError))
	{
		Impl->CurrentStatus = ERouteArtifactCurrentStatus::Invalid;
		Impl->CurrentInfo = FRouteArtifactCurrentInfo{};
		return false;
	}
	Impl->CurrentStatus = ERouteArtifactCurrentStatus::Ready;
	Impl->CurrentInfo.ArtifactSetID = OutManifest.ArtifactSetID;
	Impl->CurrentInfo.ArtifactSetDigest = OutManifest.ArtifactSetDigest;
	Impl->CurrentInfo.AssignmentBinding = OutManifest.AssignmentBinding;
	Impl->CurrentInfo.SourceDatasetDigest = OutManifest.SourceIdentity.SourceDatasetDigest;
	Impl->CurrentInfo.RouteSlotCount = OutManifest.RouteSlotCount;
	Impl->CurrentInfo.MatchedCount = OutManifest.MatchedCount;
	Impl->CurrentInfo.CurrentRoot = Current;
	Impl->CurrentInfo.CreatedAtUtc = OutManifest.CreatedAtUtc;
	return true;
}

void FRouteArtifactWorkspaceManager::Shutdown()
{
	if (HasActiveGeneration())
	{
		const FString Attempt = Impl->AttemptRoot;
		DeleteTree(Attempt);
		Impl->ClearActive(false);
	}
	else
	{
		Impl->ReleaseWriter();
	}
}

bool FRouteArtifactWorkspaceManager::SerializeReadyMarker(
	const FString& ArtifactSetID,
	const FRouteArtifactDigest256& ArtifactSetDigest,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	OutBytes.Reset();
	std::string SharedBytes;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::SerializeReadyMarker(
		RouteArtifactSharedAdapter::ToUtf8(ArtifactSetID),
		RouteArtifactSharedAdapter::ToShared(ArtifactSetDigest),
		SharedBytes,
		SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutBytes.Append(
		reinterpret_cast<const uint8*>(SharedBytes.data()),
		static_cast<int32>(SharedBytes.size()));
	return true;
}

bool FRouteArtifactWorkspaceManager::ParseReadyMarker(
	const TArray<uint8>& Bytes,
	FString& OutArtifactSetID,
	FRouteArtifactDigest256& OutArtifactSetDigest,
	FString& OutError)
{
	OutArtifactSetID.Reset();
	OutArtifactSetDigest.Reset();
	const std::string SharedBytes(
		reinterpret_cast<const char*>(Bytes.GetData()),
		static_cast<size_t>(Bytes.Num()));
	std::string SharedSetID;
	dronegcs::route_artifact::Digest256 SharedDigest;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::ParseReadyMarker(
		SharedBytes, SharedSetID, SharedDigest, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutArtifactSetID = RouteArtifactSharedAdapter::FromUtf8(SharedSetID);
	return RouteArtifactSharedAdapter::FromShared(
		SharedDigest, OutArtifactSetDigest, OutError);
}

#if WITH_DEV_AUTOMATION_TESTS
void FRouteArtifactWorkspaceManager::SetTestFailAttemptPromotion(const bool bFail)
{
	Impl->bTestFailAttemptPromotion = bFail;
}

int64 FRouteArtifactWorkspaceManager::GetTestFinalizeRouteValidationReadCount() const
{
	return Impl->TestFinalizeRouteValidationReadCount;
}

bool FRouteArtifactWorkspaceManager::ValidateCurrentEntryAllowlistForTest(
	const FRouteArtifactManifest& Manifest,
	const TArray<FString>& RelativeFiles,
	const TArray<FString>& RelativeDirectories,
	FString& OutError)
{
	return ValidateCurrentEntryAllowlist(
		Manifest, RelativeFiles, RelativeDirectories, OutError);
}

bool FRouteArtifactWorkspaceManager::TryAcquireCurrentPromotionGateForTest(
	const FString& WorkspaceRoot,
	bool& bOutRecoveredAbandonedOwner,
	FString& OutError)
{
	dronegcs::route_artifact::ArtifactReadLease Gate;
	std::string Error;
	const auto Result = Gate.TryAcquire(
		std::wstring(*WorkspaceRoot), bOutRecoveredAbandonedOwner, Error);
	if (Result == dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
	{
		OutError.Reset();
		return true;
	}
	OutError = Result == dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Busy
		? TEXT("ArtifactCurrentLeaseBusy")
		: FString(UTF8_TO_TCHAR(Error.c_str()));
	return false;
}
#endif
