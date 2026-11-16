#pragma once
#include "CoreMinimal.h"
#include "AssetDeleteModel.h"
#include "ObjectTools.h"
#include "ContentBrowserFileDataSourceAPI.h"
#include "AssetTools/ContentMutation.h"
#include "Data/FileDeletionResult.h"
#include "Data/FileDeletionPlan.h"
#include "AssetTools/NativeAssetDeletion.h"
#include "Misc/FeedbackContext.h"
namespace Durin::ContentStorage
{
	struct FFileDeletionInput { std::string Name, PhysicalPath; };

}

#include "Data/FileDeletionOperation.h"
namespace Durin::ContentStorage
{
	// One pending confirmation and one destructive attempt. Stale confirmation
	// replaces this operation's immutable plan; no request-context session map.
	class CONTENTBROWSERFILEDATASOURCE_API FFileDeletionRequest
	{
	public:
		explicit FFileDeletionRequest(const FContentMutationPolicy& Host = {}, bool InDeleteAssets = true, bool InDeleteFiles = true);
		auto PrepareDeleteItems(std::span<const FFileDeletionInput>) -> FFileDeletionPlanPtr;
		auto DeleteItems(FFileDeletionPlanPtr&, FFileDeletionHooks Hooks = {}) -> FFileDeletionResult;
		auto DismissDeletion(FFileDeletionPlanPtr Plan) -> void;
		auto ValidateForExecution() -> FAssetWriteResult;
		auto DeleteWithConfirmation(std::span<const FFileDeletionInput>, bool ShowConfirmation = true, ObjectTools::EAllowCancelDuringDelete AllowCancel = ObjectTools::EAllowCancelDuringDelete::AllowCancel) -> ObjectTools::FDeleteItemsResult;
		auto HasCommittedEffects() const -> bool { return bCommitted; }
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
	private:
		auto AnalyzeDeletion(std::span<const FFileDeletionInput>, FAssetDeletionOperation&) -> FFileDeletionPlanPtr;
		auto CanModify(std::string* Error = nullptr) const -> bool;
		auto Report(ELogLevel, std::string_view) const -> void;
		auto NotifyItemsUpdated(FContentChangeBatch) -> void;
		std::shared_ptr<FAssetDeleteModel> Model;
		bool bPresenting = false, bCommitted = false, bExecuting = false;
		FContentMutationPolicy Policy;
		FFeedbackContext::FMessageSink Diagnostics;
		FFileDeletionPlanPtr CurrentPlan;
		std::vector<FFileDeletionInput> CapturedRequest;
		std::shared_ptr<FFileDeletionOperation> Execution;
		uint64 NextSession = 0;
		int32 LastDeletedObjects = 0;
		bool DeleteAssets = true, DeleteFiles = true;
	};
}
