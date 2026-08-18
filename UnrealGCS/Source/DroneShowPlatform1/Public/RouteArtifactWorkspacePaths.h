#pragma once

#include "CoreMinimal.h"

enum class ERouteArtifactWorkspaceRootKind : uint8
{
	Development,
	ShippingLocalUser
};

namespace RouteArtifactWorkspacePaths
{
	/** Filesystem tuning only; not a Fleet or Route business limit. */
	inline constexpr int32 RouteSlotsPerShard = 1000;

	DRONESHOWPLATFORM1_API bool ResolveDefaultRoot(
		ERouteArtifactWorkspaceRootKind Kind,
		FString& OutNormalizedRoot,
		FString& OutError);

	/** A trusted configured override must be non-empty and absolute. */
	DRONESHOWPLATFORM1_API bool ResolveConfiguredRoot(
		const FString& ConfiguredRoot,
		FString& OutNormalizedRoot,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool ValidateArtifactSetID(
		const FString& ArtifactSetID,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool BuildRouteRelativePath(
		int32 RouteSlotIndex,
		FString& OutRelativePath,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool ValidateArtifactRelativePath(
		const FString& RelativePath,
		FString& OutError);

	/**
	 * Lexically resolves a validated relative path beneath a normalized root.
	 * The Workspace filesystem-safety layer rechecks junction, symlink, and
	 * reparse-point containment before real filesystem objects are used.
	 */
	DRONESHOWPLATFORM1_API bool ResolvePathUnderRoot(
		const FString& Root,
		const FString& RelativePath,
		FString& OutResolvedPath,
		FString& OutError);
}
