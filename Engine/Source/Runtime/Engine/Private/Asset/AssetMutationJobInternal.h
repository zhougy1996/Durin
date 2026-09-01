#pragma once

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Mutation.h"
#undef DURIN_ENGINE_ASSET_INTERNAL

namespace Durin
{
	// Coordinates one prepared authored mutation and retains its latest
	// publication result without owning workflow-specific state.
	struct FAssetMutationJob::FState
	{
		std::function<FAssetResult()> ResumeOperation;
		std::function<bool()> IsRecoveryRequired;
		std::function<void(FAssetMutationResultDetails&)> PopulateResultDetails;
		EAssetMutationJobState State = EAssetMutationJobState::Prepared;
		FAssetMutationResultDetails LastResult;
	};
}
