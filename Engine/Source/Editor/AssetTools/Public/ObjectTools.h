#pragma once

#include "CoreMinimal.h"
#include "AssetToolsAPI.h"

namespace Durin { class DObject; class FAssetDeleteModel; struct FAssetData; }
namespace Durin::ObjectTools
{
	enum class EAllowCancelDuringDelete : uint8 { AllowCancel, CancelNotAllowed };
	struct FDeleteItemsParameters
	{
		std::vector<DObject*> ObjectsToDelete;
		std::vector<std::string> FoldersToDelete;
		bool bShowConfirmation = true;
		EAllowCancelDuringDelete AllowCancelDuringDelete = EAllowCancelDuringDelete::AllowCancel;
	};
	struct FDeleteItemsResult { int32 ObjectsDeleted = 0, FoldersDeleted = 0; };
	using FDeletePrompt = std::function<bool(FAssetDeleteModel&, std::span<const std::string>, EAllowCancelDuringDelete)>;
	ASSETTOOLS_API auto SetDeletePrompt(FDeletePrompt Prompt) -> void;
	ASSETTOOLS_API auto ConfirmDelete(FAssetDeleteModel& Model, std::span<const std::string> Files,
		EAllowCancelDuringDelete AllowCancel = EAllowCancelDuringDelete::AllowCancel) -> bool;
	ASSETTOOLS_API auto DeleteObjects(std::span<DObject* const> ObjectsToDelete, bool bShowConfirmation = true,
		EAllowCancelDuringDelete AllowCancel = EAllowCancelDuringDelete::AllowCancel) -> int32;
	ASSETTOOLS_API auto DeleteAssets(std::span<const FAssetData> AssetsToDelete, bool bShowConfirmation = true) -> int32;
	ASSETTOOLS_API auto DeleteItems(const FDeleteItemsParameters& Parameters) -> FDeleteItemsResult;
	using FDuplicatePrompt = std::function<std::optional<std::string>(std::string_view Destination)>;
	ASSETTOOLS_API auto SetDuplicatePrompt(FDuplicatePrompt Prompt) -> void;
	// Chooses unique identities and retains successful Dirty copies; it never forces a save.
	ASSETTOOLS_API auto DuplicateObjects(std::span<DObject* const> SelectedObjects,
		std::string_view SourcePath = {}, std::string_view DestinationPath = {},
		bool bOpenDialog = true, std::vector<DObject*>* OutNewObjects = nullptr) -> void;
}
