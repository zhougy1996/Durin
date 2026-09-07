#pragma once

#include "EngineAPI.h"
#include "Asset/AssetDefinitions.h"
#include "AssetRegistry/References.h"

namespace Durin
{
	class DClass;

	struct FAssetPathResolveOptions
	{
		const DClass* ExpectedClass = nullptr;
	};

	// Current metadata/fence resolution followed by Engine loaded-type validation.
	ENGINE_API auto ResolveAssetPathForOperation(const FPackagePath& Path,
		const FAssetPathResolveOptions& Options = {}) -> FAssetPathResolveResult;
	ENGINE_API auto ResolveAssetObjectPathForOperation(const FObjectPath& Path,
		const FAssetPathResolveOptions& Options = {}) -> FObjectPathResolveResult;

	// Validates every captured alias and final package, then checks the loaded type.
	// This is a point-in-time check, not permission to reopen uncaptured artifacts.
	ENGINE_API auto ValidateResolvedAssetForOperation(const FAssetRegistrySnapshot& Snapshot,
		const FAssetPathResolveResult& Resolution, const DClass* ExpectedClass = nullptr) -> FAssetResult;
	ENGINE_API auto ValidateResolvedAssetForOperation(const FAssetRegistrySnapshot& Snapshot,
		const FObjectPathResolveResult& Resolution, const DClass* ExpectedClass = nullptr) -> FAssetResult;
}
