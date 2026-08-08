#include "Panels/ContentBrowserRefreshCoordinator.h"

namespace Durin
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
		const FGetRegistryRevision& GetRegistryRevision) -> Asset::FAssetResult
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
			const Asset::FAssetResult Result = ReconcileMountedContent();
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
		const FGetRegistryRevision& GetRegistryRevision) -> Asset::FAssetResult
	{
		const Asset::FAssetResult Result = ReconcileMountedContent();
		if (!Result)
		{
			ReconciliationState->FailedRevision = MountedContentRevision;
			return Result;
		}
		ReconciliationState->SynchronizedRevision = MountedContentRevision;
		ReconciliationState->FailedRevision.reset();
		CompleteReconciliation(
			MountedContentRevision,
			RefreshPublishedContent,
			GetRegistryRevision);
		return {};
	}

	auto FContentBrowserRefreshCoordinator::RefreshRegistryView(
		uint64 AssetRegistryRevision,
		const FRefreshPublishedContent& RefreshPublishedContent) -> void
	{
		RefreshPublishedContent();
		ObservedAssetRegistryRevision = AssetRegistryRevision;
	}

	auto FContentBrowserRefreshCoordinator::CompleteReconciliation(
		uint64 MountedContentRevision,
		const FRefreshPublishedContent& RefreshPublishedContent,
		const FGetRegistryRevision& GetRegistryRevision) -> void
	{
		RefreshPublishedContent();
		ObservedMountedContentRevision = MountedContentRevision;
		ObservedAssetRegistryRevision = GetRegistryRevision();
	}
} // namespace Durin
