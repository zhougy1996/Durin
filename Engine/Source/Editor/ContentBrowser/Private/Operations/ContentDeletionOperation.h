#pragma once

#include "CoreMinimal.h"
#include "AssetTools/AssetDeletion.h"
#include "Operations/ContentDeletionPlan.h"
#include "ContentBrowserOperationResult.h"

namespace Durin::ContentBrowser::Private
{
	// Narrow injection seam for deterministic destructive-deletion failure tests.
	struct FContentDeletionHooks
	{
		std::function<std::error_code(const std::filesystem::path&)> RemoveEntry;
		std::function<bool()> ShouldCancel;
		std::function<bool()> CanExecuteEffect;
	};

	class FContentDeletionOperation final
	{
	private:
		friend class FContentBrowserDeletionOperation;
		explicit FContentDeletionOperation(
			FContentDeletionPlanPtr InPlan,
			FAssetDeletionOperation InAssetOperation, bool InDeleteAssets, bool InDeleteFiles);

	public:
		auto Execute(FContentDeletionHooks InHooks = {}) -> bool;
		auto HasStarted() const -> bool { return bStarted; }
		auto GetResult() const -> const FAssetWriteResult& { return Result; }
		auto GetEffects() const -> const TContentBrowserOperationResult<std::string>& { return Effects; }
		auto ValidateScope() -> FAssetWriteResult;
		auto AllowPeerScopeChanges() -> void { bPeerScopeValidated = true; }

	private:
		auto ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities, bool OwnedOnly = false) -> FAssetWriteResult;
		auto DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult;

		FContentDeletionPlanPtr Plan;
		FAssetDeletionOperation AssetOperation;
		FAssetWriteResult Result;
		bool bStarted = false;
		FContentDeletionHooks Hooks;
		TContentBrowserOperationResult<std::string> Effects;
		bool DeleteAssets = true, DeleteFiles = true;
		uint64 ContributorRevision = 0;
		bool bPeerScopeValidated = false;
	};
}
