#pragma once

#include "CoreMinimal.h"

#include "DObject/AssetPath.h"

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
		// Reports committed package files, including saves with pending Registry publication.
		// The command still returns false and reports diagnostics when publication fails.
		std::function<void(const FPackagePath&)> OnSaved;
	};

}
