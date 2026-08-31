#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin
{
	enum class EAssetSaveMode : uint8
	{
		LoadedDirtyPackage,
		CanonicalResave,
	};

	struct FAssetSaveRequest
	{
		std::vector<FPackagePath> AssetPaths;
		EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage;
		FPublishAssetOperation Publish;
	};
}
