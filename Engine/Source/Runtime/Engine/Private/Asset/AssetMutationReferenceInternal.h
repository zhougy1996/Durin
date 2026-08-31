#pragma once

#include "AssetMutationJournalInternal.h"

namespace Durin::Asset::Private
{
	struct FByteReader;
	struct FMutationPackageMetadata
	{
		uint32 FormatVersion = 0;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		std::vector<FAssetPath> Dependencies;
		std::vector<FAssetPath> SoftDependencies;
	};

	// Rebuilds only the reference-index slice affected by published mutation
	// participants. AssetReference.cpp owns serialization traversal.
	auto RebuildReferenceProjectionForPublishedEntries(
		std::span<const FAssetMutationJournalEntry> Entries,
		const std::unordered_map<FAssetPath, FAssetData>& Assets,
		std::vector<FAssetReferenceEdge>& Edges,
		std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints)
		-> FAssetResult;
	auto RewritePackageReferencesForMutation(
		std::span<const std::byte> Bytes,
		std::span<const std::byte> BulkBytes,
		const FAssetPath& PackagePath,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		std::vector<std::byte>& OutBytes) -> FAssetResult;
	auto ReadMutationPackageMetadata(
		std::span<const std::byte> Bytes,
		std::span<const std::byte> BulkBytes,
		const FAssetPath& PackagePath,
		FMutationPackageMetadata& OutMetadata) -> FAssetResult;
	auto ValidateMutationPackageMetadata(
		const FMutationPackageMetadata& Metadata,
		uint64 ObjectCount,
		const FAssetPath* SourcePath = nullptr) -> FAssetResult;
	auto CollectLoadedPackageSoftReferencesForMutation(
		DPackage* Package,
		const FAssetPath& TargetPath,
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
