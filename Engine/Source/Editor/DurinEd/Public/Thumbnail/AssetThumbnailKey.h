#pragma once

#include "Thumbnail/AssetThumbnailTypes.h"

namespace Durin::Editor
{
	// Describes one package and its direct Asset Registry dependency edges.
	struct FAssetThumbnailDependencyNode
	{
		FAssetThumbnailPackageFingerprint Package;
		std::vector<FAssetPath> Dependencies;
	};

	// Contains every renderer-neutral field used to derive one persistent cache key.
	struct FAssetThumbnailKeyInput
	{
		FAssetThumbnailPackageFingerprint Asset;
		std::string RendererName;
		uint32 GeneratorSchemaVersion = 0;
		FAssetThumbnailOutputSettings Output;
		std::string PreviewFixtureIdentity;
		uint32 PreviewFixtureVersion = 0;
		uint32 ShaderContractVersion = 0;
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
	};

	// Builds a sorted transitive dependency snapshot; missing or conflicting registry data is invalid.
	DURINED_API auto BuildAssetThumbnailDependencyClosure(
		const FAssetPath& Root,
		std::span<const FAssetThumbnailDependencyNode> RegistrySnapshot,
		std::vector<FAssetThumbnailPackageFingerprint>& OutDependencies,
		std::string& OutError) -> bool;

	// Hashes explicit little-endian fields rather than formatted text or native struct memory.
	DURINED_API auto BuildAssetThumbnailCacheKey(const FAssetThumbnailKeyInput& Input) -> std::string;
} // namespace Durin::Editor
