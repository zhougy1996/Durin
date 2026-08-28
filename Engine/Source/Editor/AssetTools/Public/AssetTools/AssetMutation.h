#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin::Editor
{
	class FTransactionManager;
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
		Editor::FTransactionManager* Transactions = nullptr;
	};

	struct FAssetRedirectorFixupRequest
	{
		std::vector<FAssetPath> Redirectors;
		bool bDeleteRedirectors = true;
		Editor::FTransactionManager* Transactions = nullptr;
	};
}
