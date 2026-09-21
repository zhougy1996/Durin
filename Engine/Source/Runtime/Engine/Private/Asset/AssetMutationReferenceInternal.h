#pragma once
#include "DObject/PackageValueCodec.h"

#include "AssetMutationJournalInternal.h"

namespace Durin::AssetPrivate
{
	using FByteReader = PackagePrivate::FByteReader;
	struct FMutationPackageMetadata
	{
		uint32 FormatVersion = 0;
		std::vector<FTopLevelAssetData> TopLevelAssets;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FPackagePath RedirectDestination;
		std::vector<FPackagePath> Dependencies;
		std::vector<FPackagePath> SoftDependencies;
	};

	auto RewritePackageReferencesForMutation(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		FByteBuffer& OutBytes) -> FAssetWriteResult;
	auto ReadMutationPackageMetadata(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		FMutationPackageMetadata& OutMetadata) -> FAssetReadResult;
	auto ValidateMutationPackageMetadata(
		const FMutationPackageMetadata& Metadata,
		uint64 ObjectCount,
		const FPackagePath* SourcePath = nullptr) -> FAssetReadResult;
	auto CollectLoadedPackageSoftReferencesForMutation(
		DPackage* Package,
		const FPackagePath& TargetPath,
		std::vector<FSoftObjectPtr*>& OutValues) -> FAssetReadResult;
	auto AssetReferenceLess(
		const FAssetReferenceEdge& Left,
		const FAssetReferenceEdge& Right) -> bool;
	auto DecodeReferenceByteToolValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		FByteReader& Reader,
		const std::vector<DObject*>& Objects,
		uint32 SourceVersion) -> FAssetReadResult;
}
