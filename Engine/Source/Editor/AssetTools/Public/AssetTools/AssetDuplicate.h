#pragma once

#include "CoreMinimal.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	struct FAssetDuplicateRequest
	{
		FTopLevelAssetPath SourcePath;
		std::string DestinationDirectory;
		// Batch transfer uses an exact non-replacing target; ordinary Duplicate keeps
		// its existing unique-name policy when this value is absent.
		std::optional<FTopLevelAssetPath> DestinationPath;
	};
}
