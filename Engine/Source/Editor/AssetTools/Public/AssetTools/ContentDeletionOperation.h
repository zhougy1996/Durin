#pragma once

#include "CoreMinimal.h"
#include "AssetTools/AssetDeletion.h"
#include "AssetTools/ContentDeletionPlan.h"
#include "AssetTools/ContentDeletionResult.h"

namespace Durin::ContentStorage
{
	// Narrow injection seam for deterministic destructive-deletion failure tests.
	struct FContentDeletionHooks
	{
		std::function<std::error_code(const std::filesystem::path&)> RemoveEntry;
		std::function<bool()> ShouldCancel;
		std::function<bool()> CanExecuteEffect;
	};

	class ASSETTOOLS_API FContentDeletionOperation final
	{
	private:
		friend class FContentDeletionRequest;
		explicit FContentDeletionOperation(
			FContentDeletionPlanPtr InPlan,
			FAssetDeletionOperation InAssetOperation, bool InDeleteAssets, bool InDeleteFiles);

	public:
		auto Execute(FContentDeletionHooks InHooks = {}) -> bool;
		auto HasStarted() const -> bool { return bStarted; }
		auto GetResult() const -> const FAssetWriteResult& { return Result; }
		auto GetEffects() const -> const FContentDeletionResult& { return Effects; }
		auto ValidateScope(std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;
		auto AllowPeerScopeChanges() -> void { bPeerScopeValidated = true; }

	private:
		auto ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities, bool OwnedOnly = false, std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;
		auto DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult;

		FContentDeletionPlanPtr Plan;
		FAssetDeletionOperation AssetOperation;
		FAssetWriteResult Result;
		bool bStarted = false;
		FContentDeletionHooks Hooks;
		FContentDeletionResult Effects;
		bool DeleteAssets = true, DeleteFiles = true;
		uint64 ContributorRevision = 0;
		bool bPeerScopeValidated = false;
	};
}
