#pragma once

#include "EngineAPI.h"
#include "Asset/PackageInspection.h"
#include "Asset/AssetWriteResult.h"
#include "DObject/SoftObjectPtr.h"

namespace Durin
{
	// Byte-level path substitutions shared by Cook and authoring tools.
	struct FAssetPackageReferenceMapping
	{
		FPackagePath SourcePath;
		FPackagePath DestinationPath;
		auto operator==(const FAssetPackageReferenceMapping&) const -> bool = default;
	};

	struct FAssetRedirectorWriteMapping
	{
		FTopLevelAssetPath Source;
		FObjectPath Destination;
	};

	// Checks the live-load boundary, runtime lifecycle, and authored-package mode.
	// Call on the owner thread before preparing or publishing authored edits.
	ENGINE_API auto CheckAssetPackageMutationAllowed() -> FAssetWriteResult;
	ENGINE_API auto BuildRelocatedAssetPackageBytes(
		FByteView SourceBytes, const FPackagePath& SourcePath,
		FByteView SourceBulkBytes, const FPackagePath& DestinationPath,
		FByteBuffer& OutBytes) -> FAssetWriteResult;
	ENGINE_API auto BuildAssetRedirectorPackageBytes(
		const FPackagePath& SourcePath,
		std::span<const FAssetRedirectorWriteMapping> Mappings,
		uint32 FormatVersion, FByteBuffer& OutBytes) -> FAssetWriteResult;
	ENGINE_API auto RewriteAssetPackageReferences(
		FByteView Bytes, FByteView BulkBytes, const FPackagePath& PackagePath,
		std::span<const FAssetPackageReferenceMapping> Mappings,
		uint64 ExpectedRewriteCount, FByteBuffer& OutBytes) -> FAssetWriteResult;
	ENGINE_API auto CollectLoadedAssetPackageSoftReferences(
		DPackage* Package, const FPackagePath& TargetPath,
		std::vector<FSoftObjectPtr*>& OutValues) -> FAssetReadResult;
	ENGINE_API auto FingerprintAssetPackageBytes(
		std::string_view PhysicalPath, FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult;
}
