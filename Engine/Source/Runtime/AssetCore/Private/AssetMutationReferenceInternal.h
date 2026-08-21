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
	};

	// Rebuilds only the reference-index slice affected by published mutation
	// participants. The serialization traversal remains owned by AssetSystem
	// until the Stage 2 reference-projection split.
	auto RebuildReferenceProjectionForPublishedEntries(
		std::span<const FAssetMutationJournalEntry> Entries,
		const std::unordered_map<FAssetPath, FAssetData>& Assets,
		std::vector<FAssetReferenceEdge>& Edges,
		std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints)
		-> FAssetResult;
	auto RewritePackageReferencesForMutation(
		std::span<const uint8> Bytes,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		std::vector<uint8>& OutBytes) -> FAssetResult;
	auto ReadMutationPackageMetadata(
		std::span<const uint8> Bytes,
		FMutationPackageMetadata& OutMetadata) -> FAssetResult;
	auto ValidateMutationPackageMetadata(
		const FMutationPackageMetadata& Metadata,
		uint64 ObjectCount,
		const FAssetPath* SourcePath = nullptr) -> FAssetResult;
	auto InspectAssetPackageBytesForCatalog(
		std::string_view PhysicalPath,
		std::span<const uint8> Bytes,
		FAssetPackageInspection& OutInspection) -> FAssetResult;
	auto CollectLoadedPackageSoftReferencesForMutation(
		DPackage* Package,
		const FAssetPath& TargetPath,
		std::vector<FSoftObjectPtr*>& OutValues) -> FAssetResult;
	auto AssetReferenceLess(
		const FAssetReferenceEdge& Left,
		const FAssetReferenceEdge& Right) -> bool;
	auto ExtractAssetReferencesForCook(
		const FAssetPackageInspection& Inspection,
		std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult;
	auto DecodeReferenceByteToolValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		FByteReader& Reader,
		const std::vector<DObject*>& Objects,
		uint32 SourceVersion) -> FAssetResult;
}
