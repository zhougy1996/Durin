#pragma once
#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Misc/ContentChanges.h"
#include "ContentBrowser/ContentBrowserContracts.h"
namespace Durin::ContentBrowser::Private
{
	using FMountedContentReconciliationState = ContentBrowser::FMountedContentReconciliationState;
	class FContentBrowserRefreshCoordinator
	{
	public:
		using FReconcileMountedContent = std::function<std::expected<void, UnifiedError::FError>()>;
		using FRefreshPublishedContent = std::function<void(const FContentChangeBatch&)>;
		explicit FContentBrowserRefreshCoordinator(std::shared_ptr<FMountedContentReconciliationState> State = {})
			: ReconciliationState(State ? std::move(State) : std::make_shared<FMountedContentReconciliationState>()) {}
		auto SetPublicationSuspension(std::function<void(bool)> Notify) -> void { SetPublicationSuspended = std::move(Notify); }
		auto ObserveMountedContent(const FContentChangeBatch&) -> void;
		auto Synchronize(const FReconcileMountedContent&, const FRefreshPublishedContent&) -> std::expected<void, UnifiedError::FError>;
		auto ReconcileExplicitly(const FReconcileMountedContent&, const FRefreshPublishedContent&) -> std::expected<void, UnifiedError::FError>;
	private:
		std::shared_ptr<FMountedContentReconciliationState> ReconciliationState;
		FContentChangeBatch PendingChanges;
		bool bPending = false;
		std::function<void(bool)> SetPublicationSuspended;
	};
}
