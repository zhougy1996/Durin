#pragma once

#include "AssetRegistry/Publication.h"

namespace Durin::Asset::Private
{
	class FAssetRegistryState
	{
	public:
		FAssetRegistryState();

		auto FindAssetExact(const FAssetPath& Path) const -> FAssetCatalogEntry;
		auto FindTopLevelAssetExact(const FTopLevelAssetPath& Path) const
			-> FTopLevelAssetCatalogEntry;
		auto ResolveAssetPath(const FAssetPath& Path,
			const FAssetPathResolveOptions& Options = {}) const -> FAssetPathResolveResult;
		auto FindRedirectorsTo(const FAssetPath& Destination) const
			-> std::vector<FAssetPath>;
		auto CaptureCatalog() const -> FAssetCatalogSnapshot;
		auto CaptureDependencyClosure(
			const FAssetPath& Root) const -> FAssetDependencyClosureSnapshot;
		auto CaptureReferences() const -> FAssetReferenceIndex;
		auto CaptureSnapshot() const -> FAssetRegistrySnapshot;
		auto CapturePublication() const -> FAssetRegistryPublication;
		auto GetRevision() const -> uint64;
		auto Publish(FAssetRegistryPublication Publication) -> FAssetResult;

	private:
		mutable std::shared_mutex Mutex;
		uint64 Revision = 1;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		FAssetReferenceIndex References;
	};

	auto GetAssetRegistryState() -> FAssetRegistryState&;
	auto MarkAssetRegistryCachesDirty() -> void;
}
