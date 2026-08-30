#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin
{
	class DTransactor;
}

namespace Durin
{
	struct FAssetRelocation
	{
		FAssetPath SourcePath;
		FAssetPath DestinationPath;
	};

	struct FAssetRelocationRequest
	{
		std::vector<FAssetRelocation> Mappings;
		DTransactor* Transactions = nullptr;
	};

	struct FAssetRedirectorFixupRequest
	{
		std::vector<FAssetPath> Redirectors;
		bool bDeleteRedirectors = true;
		DTransactor* Transactions = nullptr;
	};
}
