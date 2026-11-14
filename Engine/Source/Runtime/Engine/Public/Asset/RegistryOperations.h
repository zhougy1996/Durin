#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "EngineAPI.h"
#include "AssetRegistry/References.h"

namespace Durin
{
	class DClass;

	// Reconcile exactly these authored packages after external byte publication.
	ENGINE_API auto RefreshSavedPackages(std::span<const FPackagePath> Paths) -> std::expected<void, Durin::UnifiedError::FError>;

	struct FAssetPathResolveOptions
	{
		const DClass* ExpectedClass = nullptr;
	};

	// Current metadata/fence resolution followed by Engine loaded-type validation.
	ENGINE_API auto ResolvePackagePathForOperation(const FPackagePath& Path,
		const FAssetPathResolveOptions& Options = {}) -> FPackagePathResolveResult;
	ENGINE_API auto ResolveAssetObjectPathForOperation(const FSoftObjectPath& Path,
		const FAssetPathResolveOptions& Options = {}) -> FObjectPathResolveResult;

	// Validates every captured alias and final package, then checks the loaded type.
	// This is a point-in-time check, not permission to reopen uncaptured artifacts.
	ENGINE_API auto ValidateResolvedAssetForOperation(const FAssetRegistrySnapshot& Snapshot,
		const FPackagePathResolveResult& Resolution, const DClass* ExpectedClass = nullptr) -> std::expected<void, Durin::UnifiedError::FError>;
	ENGINE_API auto ValidateResolvedAssetForOperation(const FAssetRegistrySnapshot& Snapshot,
		const FObjectPathResolveResult& Resolution, const DClass* ExpectedClass = nullptr) -> std::expected<void, Durin::UnifiedError::FError>;
}
