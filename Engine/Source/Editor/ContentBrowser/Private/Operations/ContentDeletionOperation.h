#pragma once

#include "CoreMinimal.h"
#include "AssetTools/AssetDeletion.h"
#include "Operations/ContentDeletionPlan.h"

namespace Durin::ContentBrowser::Private
{
	// Narrow injection seam for deterministic destructive-deletion failure tests.
	struct FContentDeletionHooks
	{
		std::function<std::error_code(const std::filesystem::path&)> RemoveAll;
	};

	class FContentDeletionOperation final
	{
	private:
		friend class FContentBrowserDataSource;
		explicit FContentDeletionOperation(
			FContentDeletionPlanPtr InPlan,
			FAssetDeletionOperation InAssetOperation);

	public:
		auto Execute(FContentDeletionHooks InHooks = {}) -> bool;
		auto HasStarted() const -> bool { return bStarted; }
		auto GetResult() const -> const FAssetWriteResult& { return Result; }

	private:
		auto ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities) -> FAssetWriteResult;
		auto DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult;

		FContentDeletionPlanPtr Plan;
		FAssetDeletionOperation AssetOperation;
		FAssetWriteResult Result;
		bool bStarted = false;
		FContentDeletionHooks Hooks;
	};
}
