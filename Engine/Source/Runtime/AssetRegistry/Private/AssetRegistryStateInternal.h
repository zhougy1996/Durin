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
		auto ResolveAssetObjectPath(const FObjectPath& Path,
			const FAssetPathResolveOptions& Options) const -> FObjectPathResolveResult;
		auto FindRedirectorsTo(const FPackagePath& Destination) const
			-> std::vector<FPackagePath>;
		auto CaptureCatalog() const -> FAssetCatalogSnapshot;
		auto CaptureDependencyClosure(
			const FPackagePath& Root) const -> FAssetDependencyClosureSnapshot;
		auto CaptureReferences() const -> FAssetReferenceIndex;
		auto CaptureSnapshot() const -> FAssetRegistrySnapshot;
		auto CapturePublication() const -> FAssetRegistryPublication;
		auto GetRevision() const -> uint64;
		auto CaptureChanges(uint64 FromRevision) const -> FContentChangeBatch;
		auto Publish(FAssetRegistryPublication Publication) -> FAssetRegistryResult;
		auto PublishDelta(FAssetRegistryDelta Delta) -> FAssetRegistryResult;
		auto Fence(std::span<const FPackagePath> Paths) -> void;
		auto ClearFence(std::span<const FPackagePath> Paths) -> void;
		auto IsFenced(const FPackagePath& Path) const -> bool;
		auto CaptureFences() const -> std::vector<FPackagePath>;

	private:
		// Caller holds Mutex; full ordering is materialized only for owned snapshots.
		auto CaptureReferenceEdgesLocked() const -> std::vector<FAssetPackageReferenceEdge>;
		mutable std::shared_mutex Mutex;
		uint64 Revision = 1;
		FContentChangeJournal Changes;
		std::unordered_map<FPackagePath, FAssetData> Assets;
		FAssetReferenceIndex References;
		// Source buckets allow publication to replace only the affected packages.
		std::unordered_map<FPackagePath, std::vector<FAssetPackageReferenceEdge>> ReferenceEdgesBySource;
		std::unordered_set<FPackagePath> ProjectionFences;
	};

	auto GetAssetRegistryState() -> FAssetRegistryState&;
	auto MarkAssetRegistryCachesDirty() -> void;
}
