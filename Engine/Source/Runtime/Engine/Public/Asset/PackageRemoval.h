#pragma once

#include "Asset/AssetDefinitions.h"
#include "AssetRegistry/Catalog.h"
#include "EngineAPI.h"

namespace Durin
{
	// Validates the entire expected set before retiring its resident object graphs.
	// Rejects cooked mode, stale metadata, loading/dirty packages and outside hard
	// referencers. References within the set are allowed. Main-thread authoring only.
	ENGINE_API auto ReleasePackagesForRemoval(
		std::span<const FAssetData> Packages, uint64 ExpectedRevision) -> FAssetResult;

	// Removes only matching catalog entries after their package files are absent.
	// Failure leaves catalog publication unchanged; callers fence paths after an I/O commit.
	ENGINE_API auto PublishPackageRemoval(
		std::span<const FAssetData> Packages, uint64 ExpectedRevision) -> FAssetResult;
}
