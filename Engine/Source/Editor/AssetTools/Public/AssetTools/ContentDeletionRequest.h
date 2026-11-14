#pragma once
#include "CoreMinimal.h"
#include "AssetTools/ContentMutation.h"
#include "AssetTools/ContentDeletionResult.h"
#include "AssetTools/ContentDeletionPlan.h"
#include "AssetTools/AssetDeletion.h"
#include "Misc/FeedbackContext.h"
namespace Durin::ContentStorage
{
	struct FContentDeletionInput { std::string Name, PhysicalPath; };

}

#include "AssetTools/ContentDeletionOperation.h"
namespace Durin::ContentStorage
{
	// One pending confirmation and one destructive attempt. Stale confirmation
	// replaces this operation's immutable plan; no request-context session map.
	class ASSETTOOLS_API FContentDeletionRequest
	{
	public:
		explicit FContentDeletionRequest(const FContentMutationPolicy& Host = {}, bool InDeleteAssets = true, bool InDeleteFiles = true);
		auto PrepareDeleteItems(std::span<const FContentDeletionInput>) -> FContentDeletionPlanPtr;
		auto DeleteItems(FContentDeletionPlanPtr&, FContentDeletionHooks Hooks = {}) -> FContentDeletionResult;
		auto DismissDeletion(FContentDeletionPlanPtr Plan) -> void;
		auto ValidateForExecution() -> FAssetWriteResult;
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
	private:
		auto AnalyzeDeletion(std::span<const FContentDeletionInput>, FAssetDeletionOperation&) const -> FContentDeletionPlanPtr;
		auto CanModify(std::string* Error = nullptr) const -> bool;
		auto Report(ELogLevel, std::string_view) const -> void;
		auto NotifyItemsUpdated(FContentChangeBatch) -> void;
		FContentMutationPolicy Policy;
		FFeedbackContext::FMessageSink Diagnostics;
		FContentDeletionPlanPtr CurrentPlan;
		std::vector<FContentDeletionInput> CapturedRequest;
		std::unique_ptr<FContentDeletionOperation> Execution;
		uint64 NextSession = 0;
		bool DeleteAssets = true, DeleteFiles = true;
	};
}
