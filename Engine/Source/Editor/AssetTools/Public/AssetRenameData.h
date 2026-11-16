#pragma once
#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/AssetPath.h"
namespace Durin
{
	struct FAssetRenameData
	{
		TWeakObjectPtr<DObject> Asset;
		std::string NewPackagePath, NewName;
		FSoftObjectPath OldObjectPath, NewObjectPath;
		bool bOnlyFixSoftReferences = false;
		FAssetRenameData() = default;
		ASSETTOOLS_API FAssetRenameData(TWeakObjectPtr<DObject> InAsset, std::string_view InNewPackagePath,
			std::string_view InNewName, bool OnlySoft = false);
		ASSETTOOLS_API FAssetRenameData(const FSoftObjectPath& OldPath, const FSoftObjectPath& NewPath, bool OnlySoft = false);
	};
	enum class ERedirectFixupMode : uint8 { DeleteFixedUpRedirectors, LeaveFixedUpRedirectors };
}
