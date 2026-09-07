#include "Panels/ContentBrowserRefreshCoordinator.h"

namespace Durin::Editor::ContentBrowser::Private
{
	FContentBrowserRefreshCoordinator::FContentBrowserRefreshCoordinator(
		uint64 InMountedContentRevision,
		uint64 InAssetRegistryRevision,
		std::shared_ptr<FMountedContentReconciliationState> InReconciliationState)
		: ReconciliationState(InReconciliationState
			? std::move(InReconciliationState)
			: std::make_shared<FMountedContentReconciliationState>())
	{
		Reset(InMountedContentRevision, InAssetRegistryRevision);
	}

	auto FContentBrowserRefreshCoordinator::Reset(
		uint64 InMountedContentRevision,
		uint64 InAssetRegistryRevision) -> void
	{
		ObservedMountedContentRevision = InMountedContentRevision;
		ObservedAssetRegistryRevision = InAssetRegistryRevision;
		if (!ReconciliationState)
			ReconciliationState =
				std::make_shared<FMountedContentReconciliationState>();
		if (!ReconciliationState->bInitialized)
		{
			ReconciliationState->SynchronizedRevision =
				InMountedContentRevision;
			ReconciliationState->bInitialized = true;
		}
	}

	auto FContentBrowserRefreshCoordinator::Synchronize(
		uint64 MountedContentRevision,
		uint64 AssetRegistryRevision,
		const FReconcileMountedContent& ReconcileMountedContent,
		const FRefreshPublishedContent& RefreshPublishedContent,
		const FGetRegistryRevision& GetRegistryRevision) -> FAssetResult
	{
		const bool bPanelMountedContentChanged =
			MountedContentRevision != ObservedMountedContentRevision;
		const bool bReconciliationRequired = MountedContentRevision
			!= ReconciliationState->SynchronizedRevision;
		const bool bFailedRevisionSuppressed =
			ReconciliationState->FailedRevision
			&& *ReconciliationState->FailedRevision == MountedContentRevision;
		if (bReconciliationRequired && !bFailedRevisionSuppressed)
		{
			const FAssetResult Result = ReconcileMountedContent();
			if (!Result)
			{
				ReconciliationState->FailedRevision = MountedContentRevision;
				return Result;
			}
			ReconciliationState->SynchronizedRevision = MountedContentRevision;
			ReconciliationState->FailedRevision.reset();
		}

		if (bPanelMountedContentChanged
			&& ReconciliationState->SynchronizedRevision
				== MountedContentRevision)
		{
			CompleteReconciliation(
				MountedContentRevision,
				RefreshPublishedContent,
				GetRegistryRevision);
			return {};
		}
		if (AssetRegistryRevision != ObservedAssetRegistryRevision)
			RefreshRegistryView(
				AssetRegistryRevision, RefreshPublishedContent);
		return {};
	}

	auto FContentBrowserRefreshCoordinator::ReconcileExplicitly(
		uint64 MountedContentRevision,
		const FReconcileMountedContent& ReconcileMountedContent,
		const FRefreshPublishedContent& RefreshPublishedContent,
		const FGetRegistryRevision& GetRegistryRevision) -> FAssetResult
	{
		const FAssetResult Result = ReconcileMountedContent();
		if (!Result)
		{
			ReconciliationState->FailedRevision = MountedContentRevision;
			return Result;
		}
		ReconciliationState->SynchronizedRevision = MountedContentRevision;
		ReconciliationState->FailedRevision.reset();
		RefreshPublishedContent({.bFullRefresh = true});
		ObservedMountedContentRevision = MountedContentRevision;
		ObservedAssetRegistryRevision = GetRegistryRevision();
		return {};
	}

	auto FContentBrowserRefreshCoordinator::RefreshRegistryView(
		uint64 AssetRegistryRevision,
		const FRefreshPublishedContent& RefreshPublishedContent) -> void
	{
		RefreshPublishedContent(CaptureImpact(ObservedMountedContentRevision, AssetRegistryRevision));
		ObservedAssetRegistryRevision = AssetRegistryRevision;
	}

	auto FContentBrowserRefreshCoordinator::CompleteReconciliation(
		uint64 MountedContentRevision,
		const FRefreshPublishedContent& RefreshPublishedContent,
		const FGetRegistryRevision& GetRegistryRevision) -> void
	{
		const uint64 CatalogRevision = GetRegistryRevision();
		RefreshPublishedContent(CaptureImpact(MountedContentRevision, CatalogRevision));
		ObservedMountedContentRevision = MountedContentRevision;
		ObservedAssetRegistryRevision = CatalogRevision;
	}

	auto FContentBrowserRefreshCoordinator::CaptureImpact(uint64 MountedRevision, uint64 CatalogRevision) const
		-> FContentChangeBatch
	{
		FContentChangeBatch Result;
		const auto Append = [&](uint64 From, uint64 To, const FCaptureChanges& Capture) {
			if (From == To) return;
			const auto Batch = Capture ? Capture(From) : FContentChangeBatch{.bFullRefresh = true};
			Result.bFullRefresh |= Batch.bFullRefresh || Batch.FromRevision != From || Batch.ToRevision != To;
			Result.Changes.insert(Result.Changes.end(), Batch.Changes.begin(), Batch.Changes.end());
		};
		// Apply explicit rename mappings before catalog removals of the old identity.
		Append(ObservedMountedContentRevision, MountedRevision, CaptureMounted);
		Append(ObservedAssetRegistryRevision, CatalogRevision, CaptureCatalog);
		return Result;
	}
} // namespace Durin::Editor::ContentBrowser::Private
