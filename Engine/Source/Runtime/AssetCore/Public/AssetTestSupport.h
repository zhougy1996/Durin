#pragma once

#include "AssetMutation.h"

namespace Durin::Asset
{
	ASSETCORE_API auto SerializeAssetPackageBytesForFormatForTesting(
		DPackage* Package,
		uint32 FormatVersion,
		std::vector<uint8>& OutBytes) -> FAssetResult;
	enum class EAssetRedirectorFixupFailurePoint : uint8
	{
		None,
		PreparePackage,
		PrepareStore,
		StageOriginal,
		PublishPackage,
		ApplyStore,
		Verify,
		DeleteRedirector,
		PublishRegistry,
		CompensatePackage,
		CompensateStore
	};

	ASSETCORE_API auto SetAssetRedirectorFixupFailurePointForTesting(
		EAssetRedirectorFixupFailurePoint Point,
		uint32 Occurrence = 1) -> void;

	enum class EAssetRelocationFailurePoint : uint8
	{
		None,
		PrepareOutput,
		StageOriginal,
		PublishRealAsset,
		PublishOwnedPayload,
		PublishRedirector,
		UpdateLoadedPackage,
		PublishRegistry,
		CompensateFile,
		CompensateLoadedPackage,
	};

	ASSETCORE_API auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence = 1) -> void;
	ASSETCORE_API auto FlushAssetCatalogSnapshotForTesting() -> void;
	ASSETCORE_API auto IsAssetCatalogSnapshotDirtyForTesting() -> bool;
	ASSETCORE_API auto GetAssetCatalogCacheWarningForTesting() -> std::string;
}
