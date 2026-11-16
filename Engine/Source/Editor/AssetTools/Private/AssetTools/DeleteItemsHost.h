#pragma once
#include "CoreMinimal.h"
#include "ObjectTools.h"
namespace Durin::ObjectTools::Private
{
	// Provider admission is rechecked after confirmation; native safety checks still own execution.
	using FDeleteAdmission = std::function<bool()>;
	auto DeleteObjectsAndFolders(const FDeleteItemsParameters&) -> FDeleteItemsResult;
	ASSETTOOLS_API auto ScanDeleteModel(FAssetDeleteModel& Model,
		EAllowCancelDuringDelete AllowCancel = EAllowCancelDuringDelete::CancelNotAllowed) -> bool;
	ASSETTOOLS_API auto ConfirmDelete(FAssetDeleteModel& Model, std::span<const std::string> Files,
		EAllowCancelDuringDelete AllowCancel = EAllowCancelDuringDelete::AllowCancel) -> bool;
	ASSETTOOLS_API auto DeleteObjects(FAssetDeleteModel& Model, const FDeleteAdmission& CanExecute) -> int32;
}
