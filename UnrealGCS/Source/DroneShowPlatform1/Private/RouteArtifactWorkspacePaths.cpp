#include "RouteArtifactWorkspacePaths.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "RouteArtifactSharedAdapter.h"

namespace
{
	bool ContainsNulOrControl(const FString& Value)
	{
		for (int32 CharacterIndex = 0; CharacterIndex < Value.Len(); ++CharacterIndex)
		{
			const TCHAR Character = Value[CharacterIndex];
			if (Character < TEXT(' '))
			{
				return true;
			}
		}
		return false;
	}

	bool NormalizeAbsoluteRoot(
		const FString& Input,
		FString& OutNormalizedRoot,
		FString& OutError)
	{
		OutNormalizedRoot.Reset();
		OutError.Reset();
		if (Input.IsEmpty() || Input.TrimStartAndEnd().IsEmpty() || ContainsNulOrControl(Input))
		{
			OutError = TEXT("Artifact Workspace root must be non-empty and contain no control characters.");
			return false;
		}

		FString Root = Input;
		FPaths::NormalizeFilename(Root);
		if (FPaths::IsRelative(Root))
		{
			OutError = TEXT("Artifact Workspace root must be an absolute filesystem path.");
			return false;
		}
		Root = FPaths::ConvertRelativePathToFull(Root);
		if (!FPaths::CollapseRelativeDirectories(Root))
		{
			OutError = TEXT("Artifact Workspace root contains invalid relative traversal.");
			return false;
		}
		FPaths::NormalizeDirectoryName(Root);
		if (Root.IsEmpty() || FPaths::IsDrive(Root))
		{
			OutError = TEXT("Artifact Workspace root cannot be a filesystem volume root.");
			return false;
		}
		OutNormalizedRoot = MoveTemp(Root);
		return true;
	}

}

bool RouteArtifactWorkspacePaths::ResolveDefaultRoot(
	const ERouteArtifactWorkspaceRootKind Kind,
	FString& OutNormalizedRoot,
	FString& OutError)
{
	FString Candidate;
	switch (Kind)
	{
	case ERouteArtifactWorkspaceRootKind::Development:
		Candidate = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RouteArtifacts"));
		break;
	case ERouteArtifactWorkspaceRootKind::ShippingLocalUser:
		// UE 5.5 Windows resolves UserSettingsDir to LocalAppData (or
		// LocalAppDataLow for a low-integrity process), not ProgramData.
		Candidate = FPaths::Combine(
			FPlatformProcess::UserSettingsDir(), TEXT("DroneGCS"), TEXT("ArtifactWorkspace"));
		break;
	default:
		OutNormalizedRoot.Reset();
		OutError = TEXT("Artifact Workspace root kind is unsupported.");
		return false;
	}
	return NormalizeAbsoluteRoot(Candidate, OutNormalizedRoot, OutError);
}

bool RouteArtifactWorkspacePaths::ResolveConfiguredRoot(
	const FString& ConfiguredRoot,
	FString& OutNormalizedRoot,
	FString& OutError)
{
	return NormalizeAbsoluteRoot(ConfiguredRoot, OutNormalizedRoot, OutError);
}

bool RouteArtifactWorkspacePaths::ValidateArtifactSetID(
	const FString& ArtifactSetID,
	FString& OutError)
{
	dronegcs::route_artifact::Error SharedError;
	const bool bValid = dronegcs::route_artifact::ValidateArtifactSetId(
		RouteArtifactSharedAdapter::ToUtf8(ArtifactSetID), SharedError);
	OutError = bValid ? FString{} : RouteArtifactSharedAdapter::ErrorText(SharedError);
	return bValid;
}

bool RouteArtifactWorkspacePaths::BuildRouteRelativePath(
	const int32 RouteSlotIndex,
	FString& OutRelativePath,
	FString& OutError)
{
	OutRelativePath.Reset();
	std::string SharedPath;
	dronegcs::route_artifact::Error SharedError;
	if (!dronegcs::route_artifact::BuildRouteRelativePath(
		RouteSlotIndex, SharedPath, SharedError))
	{
		OutError = RouteArtifactSharedAdapter::ErrorText(SharedError);
		return false;
	}
	OutRelativePath = RouteArtifactSharedAdapter::FromUtf8(SharedPath);
	return true;
}

bool RouteArtifactWorkspacePaths::ValidateArtifactRelativePath(
	const FString& RelativePath,
	FString& OutError)
{
	dronegcs::route_artifact::Error SharedError;
	const bool bValid = dronegcs::route_artifact::ValidateArtifactRelativePath(
		RouteArtifactSharedAdapter::ToUtf8(RelativePath), SharedError);
	OutError = bValid ? FString{} : RouteArtifactSharedAdapter::ErrorText(SharedError);
	return bValid;
}

bool RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
	const FString& Root,
	const FString& RelativePath,
	FString& OutResolvedPath,
	FString& OutError)
{
	OutResolvedPath.Reset();
	FString NormalizedRoot;
	if (!NormalizeAbsoluteRoot(Root, NormalizedRoot, OutError)
		|| !ValidateArtifactRelativePath(RelativePath, OutError))
	{
		return false;
	}

	FString Candidate = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(NormalizedRoot, RelativePath));
	if (!FPaths::CollapseRelativeDirectories(Candidate))
	{
		OutError = TEXT("Resolved Artifact path contains invalid traversal.");
		return false;
	}
	FPaths::NormalizeFilename(Candidate);
	FString RootPrefix = NormalizedRoot;
	RootPrefix.AppendChar(TEXT('/'));
#if PLATFORM_WINDOWS
	constexpr ESearchCase::Type PathCase = ESearchCase::IgnoreCase;
#else
	constexpr ESearchCase::Type PathCase = ESearchCase::CaseSensitive;
#endif
	if (!Candidate.StartsWith(RootPrefix, PathCase))
	{
		OutError = TEXT("Resolved Artifact path escapes the configured Workspace root.");
		return false;
	}
	OutResolvedPath = MoveTemp(Candidate);
	return true;
}
