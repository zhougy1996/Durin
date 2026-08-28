#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin
{
	struct FAssetDuplicateRequest
	{
		FAssetPath SourcePath;
		std::string DestinationDirectory;
		bool bSave = true;
		std::function<std::string(const FAssetPath&)> ResolvePhysicalPackagePath;
		FPublishAssetOperation Publish;
	};
}
