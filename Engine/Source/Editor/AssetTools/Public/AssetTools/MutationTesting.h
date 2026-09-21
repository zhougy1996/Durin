#pragma once

#include "AssetTools/Relocation.h"
#include "AssetTools/RedirectorFixup.h"

namespace Durin
{
	// Invoke the callback after successful staging, immediately before commit validation.
	ASSETTOOLS_API auto RelocateAssetsWithBeforeCommitForTesting(
		std::span<const FAssetRelocationMapping> Mappings,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails;
	ASSETTOOLS_API auto FixUpRedirectorsWithBeforeCommitForTesting(
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

	ASSETTOOLS_API auto SetAssetRedirectorFixupFailurePointForTesting(
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

	ASSETTOOLS_API auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence = 1
	) -> void;

}
