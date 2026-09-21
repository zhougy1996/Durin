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
		LatestMountedContentRevision = InMountedContentRevision;
		ObservedMountedContentRevision = InMountedContentRevision;
		ObservedAssetRegistryRevision = InAssetRegistryRevision;
		if (!ReconciliationState)
			ReconciliationState =
				std::make_shared<FMountedContentReconciliationState>();
		if (ReconciliationState->State == FMountedContentReconciliationState::EState::Uninitialized)
		{
			ReconciliationState->Revision =
				InMountedContentRevision;
			ReconciliationState->State = FMountedContentReconciliationState::EState::Synchronized;
		}
	}

	auto FContentBrowserRefreshCoordinator::ObserveMountedContent(uint64 Revision) -> void
	{
		if (Revision == LatestMountedContentRevision) return;
		LatestMountedContentRevision = Revision;
		// Suspend only captures that predate this mutation. A fresh explicit/catalog
		// capture remains eligible even when automatic reconciliation is suppressed.
		if (SetPublicationSuspended) SetPublicationSuspended(true);
	}

	auto FContentBrowserRefreshCoordinator::Synchronize(
		uint64 MountedContentRevision,
		uint64 AssetRegistryRevision,
		const FReconcileMountedContent& ReconcileMountedContent,
		const FRefreshPublishedContent& RefreshPublishedContent,
		const FGetRegistryRevision& GetRegistryRevision) -> FAssetReadResult
	{
		ObserveMountedContent(MountedContentRevision);
		const bool bPanelMountedContentChanged =
			MountedContentRevision != ObservedMountedContentRevision;
		if (ReconciliationState->Revision != MountedContentRevision
			|| ReconciliationState->State == FMountedContentReconciliationState::EState::Uninitialized)
		{
			const auto Result = ReconcileMountedContent();
			ReconciliationState->Revision = MountedContentRevision;
			ReconciliationState->State = Result
				? FMountedContentReconciliationState::EState::Synchronized
				: FMountedContentReconciliationState::EState::Failed;
			if (!Result) return Result;
		}

		if (bPanelMountedContentChanged
			&& ReconciliationState->State == FMountedContentReconciliationState::EState::Synchronized
			&& ReconciliationState->Revision
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
		const FGetRegistryRevision& GetRegistryRevision) -> FAssetReadResult
	{
		ObserveMountedContent(MountedContentRevision);
		const auto Result = ReconcileMountedContent();
		ReconciliationState->Revision = MountedContentRevision;
		ReconciliationState->State = Result
			? FMountedContentReconciliationState::EState::Synchronized
			: FMountedContentReconciliationState::EState::Failed;
		if (!Result) return Result;
		RefreshPublishedContent({.bFullRefresh = true});
		if (SetPublicationSuspended) SetPublicationSuspended(false);
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
		if (SetPublicationSuspended) SetPublicationSuspended(false);
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
