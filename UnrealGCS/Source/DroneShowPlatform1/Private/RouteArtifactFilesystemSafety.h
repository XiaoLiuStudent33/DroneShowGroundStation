#pragma once

#include "CoreMinimal.h"

namespace RouteArtifactFilesystemSafety
{
	/** Checks every existing object from Root through Candidate without following reparse entries. */
	bool ValidatePathNoReparse(
		const FString& Root,
		const FString& Candidate,
		bool bLeafMustExist,
		bool bLeafMustBeDirectory,
		FString& OutError);

	/** One breadth-first enumeration; reparse entries are rejected before recursion. */
	bool EnumerateTreeNoReparse(
		const FString& Root,
		TArray<FString>& OutRelativeFiles,
		TArray<FString>& OutRelativeDirectories,
		FString& OutError);

	/** Deletes only after proving the complete tree contains no reparse entry. */
	bool DeleteTreeNoReparse(const FString& Root, FString& OutError);
}
