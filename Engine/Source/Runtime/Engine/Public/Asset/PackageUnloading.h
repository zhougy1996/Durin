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

	// Collecting operation: caller establishes a safe point outside business callbacks
	// and borrowed object storage. Releases Standalone residency and collects.
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
