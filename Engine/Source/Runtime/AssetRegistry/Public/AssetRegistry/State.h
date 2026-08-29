#pragma once

#include "AssetRegistry/References.h"

namespace Durin::Asset
{
	struct FAssetRegistryPublication
	{
		uint64 ExpectedRevision = 0;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::vector<FAssetReferenceEdge> ReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> ReferenceFingerprints;
		std::vector<FAssetResult> ReferenceErrors;
		FAssetReferenceIndexStats ReferenceStats;
		std::string ReferenceCacheWarning;
		bool bReferenceIndexComplete = false;
	};

	// Owns one process-local immutable metadata state. Writers replace the whole
	// validated projection against an expected revision; readers receive values.
	class ASSETREGISTRY_API FAssetRegistryState
	{
	public:
		FAssetRegistryState();

		auto FindAssetExact(const FAssetPath& Path) const -> FAssetCatalogEntry;
		auto ResolveAssetPath(const FAssetPath& Path,
			const FAssetPathResolveOptions& Options = {}) const -> FAssetPathResolveResult;
		auto FindRedirectorsTo(const FAssetPath& Destination) const
			-> std::vector<FAssetPath>;
		auto CaptureCatalog() const -> FAssetCatalogSnapshot;
		auto CaptureReferences() const -> FAssetReferenceIndex;
		auto CapturePublication() const -> FAssetRegistryPublication;
		auto GetRevision() const -> uint64;
		auto Publish(FAssetRegistryPublication Publication) -> FAssetResult;

	private:
		mutable std::shared_mutex Mutex;
		uint64 Revision = 1;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		FAssetReferenceIndex References;
	};

	ASSETREGISTRY_API auto GetAssetRegistryState() -> FAssetRegistryState&;
}
