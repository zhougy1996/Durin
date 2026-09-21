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
		return State ? State->State : EAssetMutationJobState::Empty;
	}

	auto FAssetMutationJob::GetLastResultDetails() const
		-> FAssetMutationResultDetails
	{
		return State ? State->LastResult : FAssetMutationResultDetails{};
	}

	auto FAssetMutationJob::ResumeForward() -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return Guard;
		if (!State)
			return Error(EAssetWriteError::StaleData,
				"The asset mutation job is empty.");
		if (State->State != EAssetMutationJobState::Prepared)
		{
			FAssetWriteResult Result = Error(EAssetWriteError::StaleData,
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
			return Error(EAssetWriteError::StaleData,
				"The asset mutation job has no forward operation.");
		FAssetWriteResult Result = State->ResumeOperation();
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
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}
}
