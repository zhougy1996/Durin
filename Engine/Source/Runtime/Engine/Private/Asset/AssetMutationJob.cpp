#include "Asset/AssetWriteResult.h"
#include "AssetLiveLoadGuard.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetMutationJobInternal.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto FAssetMutationJob::GetState() const
		-> EAssetMutationJobState
	{
		return State ? State->ExecutionState : EAssetMutationJobState::Empty;
	}

	auto FAssetMutationJob::GetLastResultDetails() const
		-> FAssetMutationResultDetails
	{
		return State ? State->LastResult : FAssetMutationResultDetails{};
	}

	auto FAssetMutationJob::Execute() -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return AssetWriteResultFromRead(Guard);
		if (!State)
			return Error(EAssetWriteError::StaleData,
				"The asset mutation job is empty.");
		if (GetState() != EAssetMutationJobState::Prepared)
			return Error(EAssetWriteError::StaleData,
				"An asset mutation job can execute only once.");

		if (!State->ExecuteOperation)
			return Error(EAssetWriteError::StaleData,
				"The asset mutation job has no execution operation.");
		State->ExecutionState = EAssetMutationJobState::Executing;
		FAssetWriteResult Result = State->ExecuteOperation();
		State->ExecutionState = Result ? EAssetMutationJobState::Completed : EAssetMutationJobState::Failed;
		State->LastResult = {
			.Result = Result,
			.RegistryRevision = GetAssetCatalogRevision(),
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}
}
