#pragma once

#include "AssetMutationJournalInternal.h"

namespace Durin::AssetPrivate
{
	struct FByteReader;
	struct FMutationPackageMetadata
	{
		uint32 FormatVersion = 0;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FPackagePath RedirectDestination;
		std::vector<FPackagePath> Dependencies;
		std::vector<FPackagePath> SoftDependencies;
	};

	auto RewritePackageReferencesForMutation(
		std::span<const std::byte> Bytes,
		std::span<const std::byte> BulkBytes,
		const FPackagePath& PackagePath,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		FByteArray& OutBytes) -> FAssetResult;
	auto ReadMutationPackageMetadata(
		std::span<const std::byte> Bytes,
		std::span<const std::byte> BulkBytes,
		const FPackagePath& PackagePath,
		FMutationPackageMetadata& OutMetadata) -> FAssetResult;
	auto ValidateMutationPackageMetadata(
		const FMutationPackageMetadata& Metadata,
		uint64 ObjectCount,
		const FPackagePath* SourcePath = nullptr) -> FAssetResult;
	auto CollectLoadedPackageSoftReferencesForMutation(
		DPackage* Package,
		const FPackagePath& TargetPath,
		std::vector<FSoftObjectPtr*>& OutValues) -> FAssetResult;
	auto AssetReferenceLess(
		const FAssetReferenceEdge& Left,
		const FAssetReferenceEdge& Right) -> bool;
	auto DecodeReferenceByteToolValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		FByteReader& Reader,
		const std::vector<DObject*>& Objects,
		uint32 SourceVersion) -> FAssetResult;
}
