#pragma once

#include "CoreMinimal.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	struct FAssetDuplicateRequest
	{
		FTopLevelAssetPath SourcePath;
		std::string DestinationDirectory;
	};
}
