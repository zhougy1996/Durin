#include "Panels/ContentBrowserRefreshCoordinator.h"
namespace Durin::ContentBrowser::Private
{
	auto FContentBrowserRefreshCoordinator::ObserveMountedContent(const FContentChangeBatch& Changes) -> void
	{
		PendingChanges.bFullRefresh |= Changes.bFullRefresh;
		PendingChanges.Changes.insert(PendingChanges.Changes.end(), Changes.Changes.begin(), Changes.Changes.end());
		bPending = true;
		ReconciliationState->bPending = true;
		ReconciliationState->bFailed = false;
		if (SetPublicationSuspended) SetPublicationSuspended(true);
	}
	auto FContentBrowserRefreshCoordinator::Synchronize(const FReconcileMountedContent& Reconcile, const FRefreshPublishedContent& Refresh)
		-> std::expected<void, UnifiedError::FError>
	{
		if (ReconciliationState->bFailed && !ReconciliationState->bPending) return {};
		const bool Pending = std::exchange(bPending, false);
		auto Changes = std::exchange(PendingChanges, {});
		if (std::exchange(ReconciliationState->bPending, false))
		{
			const auto Result = Reconcile();
			ReconciliationState->bFailed = !Result;
			if (!Result)
			{
				PendingChanges.bFullRefresh |= Changes.bFullRefresh;
				PendingChanges.Changes.insert(PendingChanges.Changes.begin(), Changes.Changes.begin(), Changes.Changes.end());
				bPending |= Pending;
				return Result;
			}
		}
		if (ReconciliationState->bFailed || !Pending) return {};
		Refresh(Changes);
		if (!bPending && SetPublicationSuspended) SetPublicationSuspended(false);
		return {};
	}
	auto FContentBrowserRefreshCoordinator::ReconcileExplicitly(const FReconcileMountedContent& Reconcile, const FRefreshPublishedContent& Refresh)
		-> std::expected<void, UnifiedError::FError>
	{
		ObserveMountedContent({.bFullRefresh = true});
		return Synchronize(Reconcile, Refresh);
	}
}
