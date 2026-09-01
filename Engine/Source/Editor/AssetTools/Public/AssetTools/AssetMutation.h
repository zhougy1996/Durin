#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin
{
	struct FAssetRelocation
	{
		FPackagePath SourcePath;
		FPackagePath DestinationPath;
	};

	struct FAssetRelocationRequest
	{
		std::vector<FAssetRelocation> Mappings;
	};

	struct FAssetRedirectorFixupRequest
	{
		std::vector<FPackagePath> Redirectors;
		bool bDeleteRedirectors = true;
	};
}
