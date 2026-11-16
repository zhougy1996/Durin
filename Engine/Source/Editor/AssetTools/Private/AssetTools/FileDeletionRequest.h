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
#include "AssetTools/FileDeletionOperation.h"
namespace Durin::ContentStorage
{
	// Adapts host policy and feedback to a single owned attempt. A stale confirmation
	// prepares a replacement attempt; the request caches no model, plan or outcome.
	class ASSETTOOLS_API FFileDeletionRequest
	{
	public:
		static auto DeleteObjectsAndFolders(const ObjectTools::FDeleteItemsParameters&) -> ObjectTools::FDeleteItemsResult;
		explicit FFileDeletionRequest(const FContentMutationPolicy& Host = {}, bool InDeleteAssets = true, bool InDeleteFiles = true);
		auto PrepareDeleteItems(std::span<const FFileDeletionInput>, ObjectTools::EAllowCancelDuringDelete AllowCancel = ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed) -> FFileDeletionPlanPtr;
		auto DeleteItems(FFileDeletionPlanPtr&, FFileDeletionHooks Hooks = {}) -> FFileDeletionResult;
		auto DismissDeletion(FFileDeletionPlanPtr Plan) -> void;
		auto ValidateForExecution() -> FAssetWriteResult;
		auto DeleteWithConfirmation(std::span<const FFileDeletionInput>, bool ShowConfirmation = true, ObjectTools::EAllowCancelDuringDelete AllowCancel = ObjectTools::EAllowCancelDuringDelete::AllowCancel) -> FFileDeletionResult;
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
	private:
		auto AnalyzeDeletion(std::span<const FFileDeletionInput>, FAssetDeleteModel&, FAssetDeletionOperation&, ObjectTools::EAllowCancelDuringDelete) -> FFileDeletionPlanPtr;
		auto CanModify(std::string* Error = nullptr) const -> bool;
		auto Report(ELogLevel, std::string_view) const -> void;
		auto NotifyItemsUpdated(FContentChangeBatch) -> void;
		bool bPresenting = false, bExecuting = false;
		FContentMutationPolicy Policy;
		FFeedbackContext::FMessageSink Diagnostics;
		std::unique_ptr<FFileDeletionOperation> Execution;
		bool DeleteAssets = true, DeleteFiles = true;
	};
}
