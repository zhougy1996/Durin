#pragma once

#include "CoreMinimal.h"
#include "AssetDeleteModel.h"
#include "AssetToolsAPI.h"
#include "AssetTools/NativeAssetDeletion.h"
#include "AssetTools/FileDeletionPlan.h"
#include "AssetTools/FileDeletionResult.h"

namespace Durin::ContentStorage
{
	// Narrow injection seam for deterministic destructive-deletion failure tests.
	struct FFileDeletionHooks
	{
		std::function<std::error_code(const std::filesystem::path&)> RemoveEntry;
		std::function<bool()> ShouldCancel;
		std::function<bool()> CanExecuteEffect;
	};

	class ASSETTOOLS_API FFileDeletionOperation final
	{
	private:
		friend class FFileDeletionRequest;
		explicit FFileDeletionOperation(
			FFileDeletionPlanPtr InPlan,
			FAssetDeletionOperation InAssetOperation, bool InDeleteAssets, bool InDeleteFiles, std::shared_ptr<FAssetDeleteModel> InModel);

	public:
		auto Execute(FFileDeletionHooks InHooks = {}) -> bool;
		auto HasStarted() const -> bool { return bStarted; }
		auto GetResult() const -> const FAssetWriteResult& { return Result; }
		auto GetEffects() const -> const FFileDeletionResult& { return Effects; }
		auto ValidateScope(std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;
		auto AllowPeerScopeChanges() -> void { bPeerScopeValidated = true; }

	private:
		auto ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities, bool OwnedOnly = false, std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;
		auto DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult;

		FFileDeletionPlanPtr Plan;
		std::shared_ptr<FAssetDeleteModel> Model;
		FAssetDeletionOperation AssetOperation;
		FAssetWriteResult Result;
		bool bStarted = false;
		FFileDeletionHooks Hooks;
		FFileDeletionResult Effects;
		bool DeleteAssets = true, DeleteFiles = true;
		uint64 ContributorRevision = 0;
		bool bPeerScopeValidated = false;
	};
}
