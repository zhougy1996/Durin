#pragma once

#include "Asset/AssetWriteResult.h"

#include "EngineAPI.h"
#include "AssetRegistry/Catalog.h"

namespace Durin
{
	enum class EAssetMutationJobState : uint8
	{
		Empty,
		Prepared,
		Executing,
		Completed,
		Failed,
	};

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

	class FAssetMutationJob
	{
	public:
		ENGINE_API auto GetState() const -> EAssetMutationJobState;
		ENGINE_API auto GetLastResultDetails() const
			-> FAssetMutationResultDetails;
		// Executes once. A failed or completed job must be replaced by a newly prepared job.
		ENGINE_API auto Execute() -> FAssetWriteResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;

#if defined(DURIN_ENGINE_ASSET_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};
} // namespace Durin
