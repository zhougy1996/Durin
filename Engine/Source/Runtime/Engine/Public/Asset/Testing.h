#pragma once

#include "Asset/AssetReadResult.h"

#include "EngineAPI.h"
#include "Asset/Mutation.h"

namespace Durin
{
	// Invoke the callback after successful staging, immediately before commit validation.
	ENGINE_API auto RelocateAssetsWithBeforeCommitForTesting(
		std::span<const FAssetRelocationMapping> Mappings,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails;
	ENGINE_API auto FixUpRedirectorsWithBeforeCommitForTesting(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails;

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
	};

	ENGINE_API auto SetAssetRedirectorFixupFailurePointForTesting(
		EAssetRedirectorFixupFailurePoint Point,
		uint32 Occurrence = 1
	) -> void;

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
	};

	ENGINE_API auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence = 1
	) -> void;

	ENGINE_API auto FlushAssetCatalogSnapshotForTesting() -> void;
	ENGINE_API auto IsAssetCatalogSnapshotDirtyForTesting() -> bool;
	ENGINE_API auto GetAssetCatalogCacheWarningForTesting() -> std::string;
} // namespace Durin
