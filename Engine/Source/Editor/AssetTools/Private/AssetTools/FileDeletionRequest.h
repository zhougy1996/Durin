#pragma once
#include "CoreMinimal.h"
#include "AssetDeleteModel.h"
#include "ObjectTools.h"
#include "AssetToolsAPI.h"
#include "AssetTools/ContentMutation.h"
#include "AssetTools/FileDeletionResult.h"
#include "AssetTools/FileDeletionPlan.h"
#include "AssetTools/NativeAssetDeletion.h"
#include "Misc/FeedbackContext.h"
namespace Durin::ContentStorage
{
	struct FFileDeletionInput { std::string Name, PhysicalPath; };

}

#include "AssetTools/FileDeletionOperation.h"
namespace Durin::ContentStorage
{
	// One pending confirmation and one destructive attempt. Stale confirmation
	// replaces this operation's immutable plan; no request-context session map.
	class ASSETTOOLS_API FFileDeletionRequest
	{
	public:
		static auto DeleteObjectsAndFolders(const ObjectTools::FDeleteItemsParameters&) -> ObjectTools::FDeleteItemsResult;
		explicit FFileDeletionRequest(const FContentMutationPolicy& Host = {}, bool InDeleteAssets = true, bool InDeleteFiles = true);
		auto PrepareDeleteItems(std::span<const FFileDeletionInput>, ObjectTools::EAllowCancelDuringDelete AllowCancel = ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed) -> FFileDeletionPlanPtr;
		auto DeleteItems(FFileDeletionPlanPtr&, FFileDeletionHooks Hooks = {}) -> FFileDeletionResult;
		auto DismissDeletion(FFileDeletionPlanPtr Plan) -> void;
		auto ValidateForExecution() -> FAssetWriteResult;
		auto DeleteWithConfirmation(std::span<const FFileDeletionInput>, bool ShowConfirmation = true, ObjectTools::EAllowCancelDuringDelete AllowCancel = ObjectTools::EAllowCancelDuringDelete::AllowCancel) -> ObjectTools::FDeleteItemsResult;
		auto HasCommittedEffects() const -> bool { return bCommitted; }
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
	private:
		auto AnalyzeDeletion(std::span<const FFileDeletionInput>, FAssetDeletionOperation&, ObjectTools::EAllowCancelDuringDelete) -> FFileDeletionPlanPtr;
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
