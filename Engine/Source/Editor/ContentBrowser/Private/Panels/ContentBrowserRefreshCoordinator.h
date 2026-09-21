#pragma once

#include "Asset/AssetReadResult.h"
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
		using FReconcileMountedContent = std::function<FAssetReadResult()>;
		using FRefreshPublishedContent = std::function<void(const FContentChangeBatch&)>;
		using FGetRegistryRevision = std::function<uint64()>;

		using FCaptureChanges = std::function<FContentChangeBatch(uint64)>;
		auto SetChangeSources(FCaptureChanges Mounted, FCaptureChanges Catalog) -> void
		{ CaptureMounted = std::move(Mounted); CaptureCatalog = std::move(Catalog); }
		// Notify the model at lifecycle boundaries, rather than exposing reconciliation counters.
		auto SetPublicationSuspension(std::function<void(bool)> Notify) -> void
		{ SetPublicationSuspended = std::move(Notify); }
		auto ObserveMountedContent(uint64 Revision) -> void;
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
			-> FAssetReadResult;
		auto ReconcileExplicitly(
			uint64 MountedContentRevision,
			const FReconcileMountedContent& ReconcileMountedContent,
			const FRefreshPublishedContent& RefreshPublishedContent,
			const FGetRegistryRevision& GetRegistryRevision)
			-> FAssetReadResult;
		auto RefreshRegistryView(
			uint64 AssetRegistryRevision,
			const FRefreshPublishedContent& RefreshPublishedContent) -> void;

		auto GetObservedMountedContentRevision() const -> uint64
		{
			return ObservedMountedContentRevision;
		}

	private:
		auto CompleteReconciliation(
			uint64 MountedContentRevision,
			const FRefreshPublishedContent& RefreshPublishedContent,
			const FGetRegistryRevision& GetRegistryRevision) -> void;

		auto CaptureImpact(uint64 MountedRevision, uint64 CatalogRevision) const -> FContentChangeBatch;
		FCaptureChanges CaptureMounted;
		FCaptureChanges CaptureCatalog;
		std::function<void(bool)> SetPublicationSuspended;
		uint64 LatestMountedContentRevision = 0;
		uint64 ObservedMountedContentRevision = 0;
		uint64 ObservedAssetRegistryRevision = 0;
		std::shared_ptr<FMountedContentReconciliationState> ReconciliationState;
	};
} // namespace Durin::Editor::ContentBrowser::Private
