#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin
{
	struct FAssetDuplicateRequest
	{
		FPackagePath SourcePath;
		std::string DestinationDirectory;
		bool bSave = true;
		std::function<std::string(const FPackagePath&)> ResolvePhysicalPackagePath;
		FPublishAssetOperation Publish;
	};
}
