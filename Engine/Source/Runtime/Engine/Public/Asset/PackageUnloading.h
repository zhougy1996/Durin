#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetReadResult.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectFwd.h"
#include "EngineAPI.h"

namespace Durin
{
	// Explicit package-management tools only. Ordinary load ownership uses streamable handles.
	enum class EAssetPackageUnloadPolicy : uint8 { RejectUnsaved, DiscardUnsaved };

	// Attempts to release Standalone residency and collect an unreferenced package.
	// Returns InUse and restores residency when a live strong reference keeps it reachable.
	ENGINE_API auto UnloadPackage(
		const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved
	)
		-> FAssetReadResult;
	ENGINE_API auto UnloadPackage(
		DPackage* Package,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved
	)
		-> FAssetReadResult;
}
