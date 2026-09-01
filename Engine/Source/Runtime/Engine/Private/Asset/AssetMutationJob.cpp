#include "AssetRuntimeStateInternal.h"
#include "AssetMutationJobInternal.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto FAssetMutationJob::GetState() const
		-> EAssetMutationJobState
	{
		return State ? State->State : EAssetMutationJobState::Empty;
	}

	auto FAssetMutationJob::GetLastResultDetails() const
		-> FAssetMutationResultDetails
	{
		return State ? State->LastResult : FAssetMutationResultDetails{};
	}

	auto FAssetMutationJob::ResumeForward() -> FAssetResult
	{
		if (!State)
			return Error(EAssetError::StaleData,
				"The asset mutation job is empty.");
		if (State->State != EAssetMutationJobState::Prepared)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"Only a prepared asset mutation job can resume forward.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = GetAssetCatalogRevision(),
				.bForwardResumable = false,
			};
			return Result;
		}

		if (!State->ResumeOperation)
			return Error(EAssetError::StaleData,
				"The asset mutation job has no forward operation.");
		FAssetResult Result = State->ResumeOperation();
		const bool bRecoveryRequired = State->IsRecoveryRequired
			&& State->IsRecoveryRequired();
		if (Result)
			State->State = EAssetMutationJobState::Completed;
		else if (bRecoveryRequired)
			State->State = EAssetMutationJobState::RecoveryRequired;
		State->LastResult = {
			.Result = Result,
			.State = State->State,
			.RegistryRevision = GetAssetCatalogRevision(),
			.bForwardResumable = !Result && !bRecoveryRequired,
			.bRecoveryRequired = bRecoveryRequired,
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}
}
