#pragma once

#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "DObject/AssetPath.h"

#include "Asset/AssetWriteResult.h"


namespace Durin
{
	// Snapshot of private byte-transform evidence for native characterization only.
	struct FPackageMutationTestResult
	{
		FAssetWriteResult Result;
		std::vector<std::filesystem::path> AffectedFiles;
		std::vector<std::filesystem::path> BackupLocations;
	};

	struct FPackageRelocationTestMapping
	{
		FPackagePath SourcePath, DestinationPath;
		auto operator==(const FPackageRelocationTestMapping&) const -> bool = default;
	};
	enum class EPackageAliasFixupTestMode : uint8 { RewriteOnly, RewriteAndDelete };
	ASSETTOOLS_API auto RelocatePackageFilesForTesting(std::span<const FPackageRelocationTestMapping> Mappings) -> FPackageMutationTestResult;
	ASSETTOOLS_API auto FixupPackageFilesForTesting(std::span<const FPackagePath> Paths, EPackageAliasFixupTestMode Mode) -> FPackageMutationTestResult;

	// Invoke the callback after successful staging, immediately before commit validation.
	ASSETTOOLS_API auto RelocatePackageFilesWithBeforeCommitForTesting(
		std::span<const FPackageRelocationTestMapping> Mappings,
		const std::function<void()>& BeforeCommit) -> FPackageMutationTestResult;
	ASSETTOOLS_API auto FixupPackageFilesWithBeforeCommitForTesting(
		std::span<const FPackagePath> Redirectors,
		EPackageAliasFixupTestMode Mode,
		const std::function<void()>& BeforeCommit) -> FPackageMutationTestResult;

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
