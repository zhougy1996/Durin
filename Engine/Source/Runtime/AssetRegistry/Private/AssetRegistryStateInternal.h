#pragma once

#include "AssetRegistry/Publication.h"

namespace Durin::AssetPrivate
{
	class FAssetRegistryState
	{
	public:
		FAssetRegistryState();

		auto FindAssetExact(const FPackagePath& Path) const -> FAssetCatalogEntry;
		auto FindTopLevelAssetExact(const FTopLevelAssetPath& Path) const
			-> FTopLevelAssetCatalogEntry;
		auto ResolveAssetPath(const FPackagePath& Path,
			const FAssetPathResolveOptions& Options = {}) const -> FAssetPathResolveResult;
		auto FindRedirectorsTo(const FPackagePath& Destination) const
			-> std::vector<FPackagePath>;
		auto CaptureCatalog() const -> FAssetCatalogSnapshot;
		auto CaptureDependencyClosure(
			const FPackagePath& Root) const -> FAssetDependencyClosureSnapshot;
		auto CaptureReferences() const -> FAssetReferenceIndex;
		auto CaptureSnapshot() const -> FAssetRegistrySnapshot;
		auto CapturePublication() const -> FAssetRegistryPublication;
		auto GetRevision() const -> uint64;
		auto Publish(FAssetRegistryPublication Publication) -> FAssetRegistryResult;
		auto Fence(std::span<const FPackagePath> Paths) -> void;
		auto ClearFence(std::span<const FPackagePath> Paths) -> void;
		auto IsFenced(const FPackagePath& Path) const -> bool;
		auto CaptureFences() const -> std::vector<FPackagePath>;

	private:
		mutable std::shared_mutex Mutex;
		uint64 Revision = 1;
		std::unordered_map<FPackagePath, FAssetData> Assets;
		FAssetReferenceIndex References;
		std::unordered_set<FPackagePath> ProjectionFences;
	};

	auto GetAssetRegistryState() -> FAssetRegistryState&;
	auto MarkAssetRegistryCachesDirty() -> void;
}
