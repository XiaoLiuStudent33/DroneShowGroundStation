#include "RouteArtifactFilesystemSafety.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspacePaths.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
	bool QueryObject(
		const FString& Path,
		bool& bOutExists,
		bool& bOutDirectory,
		bool& bOutReparse,
		FString& OutError)
	{
		bOutExists = false;
		bOutDirectory = false;
		bOutReparse = false;
#if PLATFORM_WINDOWS
		const DWORD Attributes = ::GetFileAttributesW(*Path);
		if (Attributes == INVALID_FILE_ATTRIBUTES)
		{
			const DWORD Error = ::GetLastError();
			if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND)
			{
				return true;
			}
			OutError = TEXT("FilesystemAttributeQueryFailed.");
			return false;
		}
		bOutExists = true;
		bOutDirectory = (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		bOutReparse = (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
		return true;
#else
		IFileManager& Files = IFileManager::Get();
		bOutDirectory = Files.DirectoryExists(*Path);
		bOutExists = bOutDirectory || Files.FileExists(*Path);
		return true;
#endif
	}

	FString NormalizeAbsolute(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Result);
		FPaths::CollapseRelativeDirectories(Result);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	bool IsUnderOrEqual(const FString& Root, const FString& Candidate)
	{
#if PLATFORM_WINDOWS
		constexpr ESearchCase::Type PathCase = ESearchCase::IgnoreCase;
#else
		constexpr ESearchCase::Type PathCase = ESearchCase::CaseSensitive;
#endif
		return Candidate.Equals(Root, PathCase)
			|| Candidate.StartsWith(Root + TEXT("/"), PathCase);
	}

	FString RelativeTo(const FString& Root, const FString& Path)
	{
		FString Relative = Path;
		FPaths::MakePathRelativeTo(Relative, *(Root + TEXT("/")));
		FPaths::NormalizeFilename(Relative);
		return Relative;
	}
}

bool RouteArtifactFilesystemSafety::ValidatePathNoReparse(
	const FString& Root,
	const FString& Candidate,
	const bool bLeafMustExist,
	const bool bLeafMustBeDirectory,
	FString& OutError)
{
	OutError.Reset();
	FString NormalizedRoot;
	if (!RouteArtifactWorkspacePaths::ResolveConfiguredRoot(
		Root, NormalizedRoot, OutError))
	{
		return false;
	}
	const FString NormalizedCandidate = NormalizeAbsolute(Candidate);
	if (!IsUnderOrEqual(NormalizedRoot, NormalizedCandidate))
	{
		OutError = TEXT("FilesystemPathEscapesWorkspaceRoot.");
		return false;
	}

	TArray<FString> PathsToCheck;
	PathsToCheck.Add(NormalizedRoot);
	if (!NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase))
	{
		const FString Relative = RelativeTo(NormalizedRoot, NormalizedCandidate);
		TArray<FString> Segments;
		Relative.ParseIntoArray(Segments, TEXT("/"), true);
		FString Accumulated = NormalizedRoot;
		for (const FString& Segment : Segments)
		{
			Accumulated = FPaths::Combine(Accumulated, Segment);
			FPaths::NormalizeFilename(Accumulated);
			PathsToCheck.Add(Accumulated);
		}
	}

	for (int32 Index = 0; Index < PathsToCheck.Num(); ++Index)
	{
		bool bExists = false;
		bool bDirectory = false;
		bool bReparse = false;
		if (!QueryObject(PathsToCheck[Index], bExists, bDirectory, bReparse, OutError))
		{
			return false;
		}
		if (!bExists)
		{
			if (bLeafMustExist)
			{
				OutError = TEXT("RequiredWorkspacePathDoesNotExist.");
				return false;
			}
			// A missing component proves that no deeper on-disk component can be
			// a reparse point yet. Callers revalidate after creating the suffix.
			return true;
		}
		if (bReparse)
		{
			OutError = TEXT("ReparsePointDetected.");
			return false;
		}
		const bool bLeaf = Index == PathsToCheck.Num() - 1;
		if ((!bLeaf && !bDirectory) || (bLeaf && bLeafMustBeDirectory && !bDirectory))
		{
			OutError = TEXT("WorkspacePathObjectTypeMismatch.");
			return false;
		}
	}
	return true;
}

bool RouteArtifactFilesystemSafety::EnumerateTreeNoReparse(
	const FString& Root,
	TArray<FString>& OutRelativeFiles,
	TArray<FString>& OutRelativeDirectories,
	FString& OutError)
{
	OutRelativeFiles.Reset();
	OutRelativeDirectories.Reset();
	const FString NormalizedRoot = NormalizeAbsolute(Root);
	if (!ValidatePathNoReparse(
		NormalizedRoot, NormalizedRoot, true, true, OutError))
	{
		return false;
	}

	TArray<FString> PendingDirectories;
	PendingDirectories.Add(NormalizedRoot);
	for (int32 PendingIndex = 0; PendingIndex < PendingDirectories.Num(); ++PendingIndex)
	{
		const FString Directory = PendingDirectories[PendingIndex];
		bool bIterationSucceeded = true;
		const bool bVisited = IFileManager::Get().IterateDirectory(
			*Directory,
			[&](const TCHAR* FilenameOrDirectory, const bool bIsDirectory)
			{
				const FString Entry = NormalizeAbsolute(FilenameOrDirectory);
				FString EntryError;
				if (!ValidatePathNoReparse(
					NormalizedRoot, Entry, true, bIsDirectory, EntryError))
				{
					OutError = EntryError;
					bIterationSucceeded = false;
					return false;
				}
				const FString Relative = RelativeTo(NormalizedRoot, Entry);
				if (bIsDirectory)
				{
					OutRelativeDirectories.Add(Relative);
					PendingDirectories.Add(Entry);
				}
				else
				{
					OutRelativeFiles.Add(Relative);
				}
				return true;
			});
		if (!bVisited || !bIterationSucceeded)
		{
			if (OutError.IsEmpty()) OutError = TEXT("WorkspaceDirectoryEnumerationFailed.");
			return false;
		}
	}
	return true;
}

bool RouteArtifactFilesystemSafety::DeleteTreeNoReparse(
	const FString& Root,
	FString& OutError)
{
	OutError.Reset();
	IFileManager& Files = IFileManager::Get();
	if (!Files.DirectoryExists(*Root))
	{
		return true;
	}
	TArray<FString> FilesInTree;
	TArray<FString> DirectoriesInTree;
	if (!EnumerateTreeNoReparse(Root, FilesInTree, DirectoriesInTree, OutError))
	{
		return false;
	}
	if (!Files.DeleteDirectory(*Root, false, true))
	{
		OutError = TEXT("WorkspaceDirectoryDeleteFailed.");
		return false;
	}
	return true;
}
