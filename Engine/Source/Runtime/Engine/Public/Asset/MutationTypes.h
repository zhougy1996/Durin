#pragma once

#include "Asset/AssetWriteResult.h"

#include "EngineAPI.h"
#include "AssetRegistry/Catalog.h"

namespace Durin
{
	struct FAssetMutationResultDetails
	{
		FAssetWriteResult Result;
		uint64 RegistryRevision = 0;
		std::vector<std::filesystem::path> AffectedFiles;
		std::vector<std::filesystem::path> BackupLocations;
		std::vector<FPackagePath> RewrittenPaths;
		std::vector<FPackagePath> RetainedPaths;
		std::vector<FPackagePath> DeletedPaths;
		std::vector<FPackagePath> SkippedPaths;
		std::vector<FPackagePath> FailedPaths;
	};

} // namespace Durin
