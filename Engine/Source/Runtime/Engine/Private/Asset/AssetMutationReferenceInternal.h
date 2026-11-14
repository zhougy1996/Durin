#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "Asset/Mutation.h"
#include "Asset/PackageEditing.h"
#include "Asset/References.h"
#include "AssetRegistry/Catalog.h"
#include "AssetPackageFingerprintInternal.h"

namespace Durin::AssetPrivate
{
	struct FMutationPackageMetadata
	{
		uint32 FormatVersion = 0;
		std::vector<FAssetData> TopLevelAssets;
		std::vector<FPackagePath> Dependencies;
		std::vector<FPackagePath> SoftDependencies;
		std::vector<FPackagePath> BuildDependencies;
	};

	auto RewritePackageReferencesForMutation(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		std::span<const FAssetPackageReferenceMapping> Mappings,
		uint64 ExpectedRewriteCount,
		FByteBuffer& OutBytes) -> FAssetWriteResult;
	auto ReadMutationPackageMetadata(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		FMutationPackageMetadata& OutMetadata) -> std::expected<void, Durin::UnifiedError::FError>;
	auto ValidateMutationPackageMetadata(
		const FMutationPackageMetadata& Metadata,
		uint64 ObjectCount,
		const FPackagePath* SourcePath = nullptr) -> std::expected<void, Durin::UnifiedError::FError>;
	auto CollectLoadedPackageSoftReferencesForMutation(
		DPackage* Package,
		const FPackagePath& TargetPath,
		std::vector<FSoftObjectPtr*>& OutValues) -> std::expected<void, Durin::UnifiedError::FError>;
}
