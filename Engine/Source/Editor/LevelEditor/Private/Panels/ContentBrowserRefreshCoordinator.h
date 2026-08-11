#pragma once

#include "AssetSystem.h"

namespace Durin::Editor::Level
{
	struct FMountedContentReconciliationState
	{
		uint64 SynchronizedRevision = 0;
		std::optional<uint64> FailedRevision;
		bool bInitialized = false;
	};

	// Separates mounted-filesystem reconciliation from registry-only view refresh.
	// A failed mounted revision is retained but suppressed until an explicit retry
	// or a later revision arrives, preventing an automatic per-frame scan loop.
	class FContentBrowserRefreshCoordinator
	{
	public:
		using FReconcileMountedContent = std::function<Asset::FAssetResult()>;
		using FRefreshPublishedContent = std::function<void()>;
		using FGetRegistryRevision = std::function<uint64()>;

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
			-> Asset::FAssetResult;
		auto ReconcileExplicitly(
			uint64 MountedContentRevision,
			const FReconcileMountedContent& ReconcileMountedContent,
			const FRefreshPublishedContent& RefreshPublishedContent,
			const FGetRegistryRevision& GetRegistryRevision)
			-> Asset::FAssetResult;
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

		uint64 ObservedMountedContentRevision = 0;
		uint64 ObservedAssetRegistryRevision = 0;
		std::shared_ptr<FMountedContentReconciliationState> ReconciliationState;
	};
} // namespace Durin::Editor::Level
