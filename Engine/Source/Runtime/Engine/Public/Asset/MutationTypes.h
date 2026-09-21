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
		Completed,
		RecoveryRequired,
	};

	struct FAssetMutationResultDetails
	{
		FAssetWriteResult Result;
		EAssetMutationJobState State = EAssetMutationJobState::Empty;
		uint64 RegistryRevision = 0;
		bool bForwardResumable = false;
		auto IsRecoveryRequired() const -> bool { return State == EAssetMutationJobState::RecoveryRequired; }
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
		ENGINE_API auto ResumeForward() -> FAssetWriteResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;

#if defined(DURIN_ENGINE_ASSET_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};
} // namespace Durin
