#pragma once

#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "AssetTools/ContentMutation.h"
#include "AssetTools/FileDeletionResult.h"
#include "ObjectTools.h"

namespace Durin::ContentStorage
{
	struct FFileDeletionInput { std::string Name, PhysicalPath; };
	// Native failure/cancellation seam; production callers use the default physical writer.
	struct FFileDeletionHooks
	{
		std::function<std::error_code(const std::filesystem::path&)> RemoveEntry;
		std::function<bool()> ShouldCancel;
		std::function<bool()> CanExecuteEffect;
	};

	// One model-backed command. No caller-owned confirmation or execution capability.
	ASSETTOOLS_API auto DeleteFiles(std::span<const FFileDeletionInput>, const FContentMutationPolicy&,
		bool DeleteAssets = true, bool DeleteOrdinaryFiles = true, bool ShowConfirmation = true,
		ObjectTools::EAllowCancelDuringDelete AllowCancel = ObjectTools::EAllowCancelDuringDelete::AllowCancel) -> FFileDeletionResult;
}
