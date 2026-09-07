#pragma once

#include "Asset/AssetDefinitions.h"
#include "AssetRegistry/ContentChanges.h"
#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin::Editor::ContentBrowser::Private
{
	using FMountedContentReconciliationState = ::Durin::Editor::ContentBrowser::FMountedContentReconciliationState;

	// Separates mounted-filesystem reconciliation from registry-only view refresh.
	// A failed mounted revision is retained but suppressed until an explicit retry
	// or a later revision arrives, preventing an automatic per-frame scan loop.
	class FContentBrowserRefreshCoordinator
	{
	public:
		using FReconcileMountedContent = std::function<FAssetResult()>;
		using FRefreshPublishedContent = std::function<void(const FContentChangeBatch&)>;
		using FGetRegistryRevision = std::function<uint64()>;

		using FCaptureChanges = std::function<FContentChangeBatch(uint64)>;
		auto SetChangeSources(FCaptureChanges Mounted, FCaptureChanges Catalog) -> void
		{ CaptureMounted = std::move(Mounted); CaptureCatalog = std::move(Catalog); }
		FContentBrowserRefreshCoordinator() = default;
		FContentBrowserRefreshCoordinator(
			uint64 InMountedContentRevision,
			uint64 InAssetRegistryRevision,
			std::shared_ptr<FMountedContentReconciliationState>
				InReconciliationState = {});

		auto Reset(
			uint64 InMountedContentRevision,
			uint64 InAssetRegistryRevision) -> void;
		auto Synchronize(
			uint64 MountedContentRevision,
			uint64 AssetRegistryRevision,
			const FReconcileMountedContent& ReconcileMountedContent,
			const FRefreshPublishedContent& RefreshPublishedContent,
			const FGetRegistryRevision& GetRegistryRevision)
			-> FAssetResult;
		auto ReconcileExplicitly(
			uint64 MountedContentRevision,
			const FReconcileMountedContent& ReconcileMountedContent,
			const FRefreshPublishedContent& RefreshPublishedContent,
			const FGetRegistryRevision& GetRegistryRevision)
			-> FAssetResult;
		auto RefreshRegistryView(
			uint64 AssetRegistryRevision,
			const FRefreshPublishedContent& RefreshPublishedContent) -> void;

		auto GetObservedMountedContentRevision() const -> uint64
		{
			return ObservedMountedContentRevision;
		}
		auto GetObservedAssetRegistryRevision() const -> uint64
		{
			return ObservedAssetRegistryRevision;
		}

	private:
		auto CompleteReconciliation(
			uint64 MountedContentRevision,
			const FRefreshPublishedContent& RefreshPublishedContent,
			const FGetRegistryRevision& GetRegistryRevision) -> void;

		auto CaptureImpact(uint64 MountedRevision, uint64 CatalogRevision) const -> FContentChangeBatch;
		FCaptureChanges CaptureMounted;
		FCaptureChanges CaptureCatalog;
		uint64 ObservedMountedContentRevision = 0;
		uint64 ObservedAssetRegistryRevision = 0;
		std::shared_ptr<FMountedContentReconciliationState> ReconciliationState;
	};
} // namespace Durin::Editor::ContentBrowser::Private
