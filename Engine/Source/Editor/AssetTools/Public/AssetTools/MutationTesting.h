#pragma once

#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "DObject/AssetPath.h"

#include "AssetTools/MutationTypes.h"


namespace Durin
{
	struct FPackageRelocationTestMapping
	{
		FPackagePath SourcePath, DestinationPath;
		auto operator==(const FPackageRelocationTestMapping&) const -> bool = default;
	};
	enum class EPackageAliasFixupTestMode : uint8 { RewriteOnly, RewriteAndDelete };
	ASSETTOOLS_API auto RelocatePackageFilesForTesting(std::span<const FPackageRelocationTestMapping> Mappings) -> FAssetMutationResultDetails;
	ASSETTOOLS_API auto FixupPackageFilesForTesting(std::span<const FPackagePath> Paths, EPackageAliasFixupTestMode Mode) -> FAssetMutationResultDetails;

	// Invoke the callback after successful staging, immediately before commit validation.
	ASSETTOOLS_API auto RelocatePackageFilesWithBeforeCommitForTesting(
		std::span<const FPackageRelocationTestMapping> Mappings,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails;
	ASSETTOOLS_API auto FixupPackageFilesWithBeforeCommitForTesting(
		std::span<const FPackagePath> Redirectors,
		EPackageAliasFixupTestMode Mode,
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
