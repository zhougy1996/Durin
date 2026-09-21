#pragma once

#include "Asset/AssetWriteResult.h"

namespace Durin
{
	struct FAssetMutationResultDetails
	{
		FAssetWriteResult Result;
		std::vector<std::filesystem::path> AffectedFiles;
		std::vector<std::filesystem::path> BackupLocations;
	};

} // namespace Durin
