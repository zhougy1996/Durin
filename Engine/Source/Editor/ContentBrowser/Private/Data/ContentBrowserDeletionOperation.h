#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserOperationResult.h"
#include "ContentBrowserOperationContext.h"
#include "Operations/ContentDeletionOperation.h"
#include "Misc/FeedbackContext.h"

namespace Durin::ContentBrowser::Private
{
	// One pending confirmation and one destructive attempt. Stale confirmation
	// replaces this operation's immutable plan; no request-context session map.
	class FContentBrowserDeletionOperation
	{
	public:
		explicit FContentBrowserDeletionOperation(const FContentBrowserOperationContext& Host = {}, bool InDeleteAssets = true, bool InDeleteFiles = true);
		auto PrepareDeleteItems(std::span<const FContentBrowserNativeItem>) -> FContentDeletionPlanPtr;
		auto DeleteItems(FContentDeletionPlanPtr&, FContentDeletionHooks Hooks = {}) -> TContentBrowserOperationResult<std::string>;
		auto DismissDeletion(FContentDeletionPlanPtr Plan) -> void;
		auto ValidateForExecution() -> FAssetWriteResult;
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
	private:
		auto AnalyzeDeletion(std::span<const FContentBrowserNativeItem>, FAssetDeletionOperation&) const -> FContentDeletionPlanPtr;
		auto CanModify(std::string* Error = nullptr) const -> bool;
		auto Report(ELogLevel, std::string_view) const -> void;
		auto NotifyItemsUpdated(FContentChangeBatch) -> void;
		FContentBrowserOperationContext Policy;
		FFeedbackContext::FMessageSink Diagnostics;
		FContentDeletionPlanPtr CurrentPlan;
		std::vector<FContentBrowserNativeItem> CapturedRequest;
		std::unique_ptr<FContentDeletionOperation> Execution;
		uint64 NextSession = 0;
		bool DeleteAssets = true, DeleteFiles = true;
	};
}
