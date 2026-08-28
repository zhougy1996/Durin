#pragma once

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Mutation.h"
#undef DURIN_ENGINE_ASSET_INTERNAL

namespace Durin::Asset
{
	// Coordinates one prepared authored mutation and retains its latest
	// publication result without owning workflow-specific transaction state.
	struct FAssetMutationTransaction::FState
	{
		FAssetMutationSummary Summary;
		std::function<FAssetResult()> CommitOperation;
		std::function<FAssetResult()> UndoOperation;
		std::function<FAssetResult()> RedoOperation;
		std::function<bool()> IsRecoveryRequired;
		std::function<void(FAssetMutationResultDetails&)> PopulateResultDetails;
		EAssetMutationTransactionState State =
			EAssetMutationTransactionState::Prepared;
		FAssetMutationResultDetails LastResult;
	};
}
