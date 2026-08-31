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
		FPackagePath SourcePath;
		FPackagePath DestinationPath;
	};

	struct FAssetRelocationRequest
	{
		std::vector<FAssetRelocation> Mappings;
		DTransactor* Transactions = nullptr;
	};

	struct FAssetRedirectorFixupRequest
	{
		std::vector<FPackagePath> Redirectors;
		bool bDeleteRedirectors = true;
		DTransactor* Transactions = nullptr;
	};
}
