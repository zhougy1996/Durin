#pragma once

#include "CoreMinimal.h"

#include "DObject/DObjectFwd.h"
#include "AssetToolsAPI.h"

namespace Durin::PackageTools
{
	// An empty batch succeeds. Otherwise true means at least one eligible package was processed.
	// OutErrorMessage identifies skipped or still-referenced packages.
	ASSETTOOLS_API auto UnloadPackages(std::span<DPackage* const> Packages,
		std::string& OutErrorMessage, bool bUnloadDirtyPackages = false) -> bool;

	ASSETTOOLS_API auto UnloadPackages(std::span<DPackage* const> Packages) -> bool;
}
