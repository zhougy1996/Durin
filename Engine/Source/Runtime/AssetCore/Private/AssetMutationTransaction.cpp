#include "AssetRuntimeStateInternal.h"
#include "AssetMutationTransactionInternal.h"

namespace Durin::Asset
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto FAssetMutationTransaction::GetSummary() const
		-> const FAssetMutationSummary&
	{
		static const FAssetMutationSummary EmptySummary;
		return State ? State->Summary : EmptySummary;
	}

	auto FAssetMutationTransaction::GetState() const
		-> EAssetMutationTransactionState
	{
		return State ? State->State : EAssetMutationTransactionState::Empty;
	}

	auto FAssetMutationTransaction::GetLastResultDetails() const
		-> FAssetMutationResultDetails
	{
		return State ? State->LastResult : FAssetMutationResultDetails{};
	}

	auto FAssetMutationTransaction::Commit() -> FAssetResult
	{
		if (!State)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction is empty.");
		if (State->State != EAssetMutationTransactionState::Prepared)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"Only a prepared asset mutation transaction can be committed.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}

		if (!State->CommitOperation)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction has no commit operation.");
		FAssetResult Result = State->CommitOperation();
		const bool bRecoveryRequired = State->IsRecoveryRequired
			&& State->IsRecoveryRequired();
		if (Result)
			State->State = EAssetMutationTransactionState::Committed;
		else if (bRecoveryRequired)
			State->State = EAssetMutationTransactionState::RecoveryRequired;
		State->LastResult = {
			.Result = Result,
			.State = State->State,
			.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
			.bStateRestored = !Result && !bRecoveryRequired,
			.bRecoveryRequired = bRecoveryRequired,
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}

	auto FAssetMutationTransaction::Undo() -> FAssetResult
	{
		if (!State)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction is empty.");
		if (State->State != EAssetMutationTransactionState::Committed)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"Only a committed asset mutation transaction can be undone.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}
		if (!State->UndoOperation)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"This asset mutation does not support editor-history undo.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}

		FAssetResult Result = State->UndoOperation();
		const bool bRecoveryRequired = State->IsRecoveryRequired
			&& State->IsRecoveryRequired();
		if (Result)
			State->State = EAssetMutationTransactionState::Undone;
		else if (bRecoveryRequired)
			State->State = EAssetMutationTransactionState::RecoveryRequired;
		State->LastResult = {
			.Result = Result,
			.State = State->State,
			.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
			.bStateRestored = !Result && !bRecoveryRequired,
			.bRecoveryRequired = bRecoveryRequired,
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}

	auto FAssetMutationTransaction::Redo() -> FAssetResult
	{
		if (!State)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction is empty.");
		if (State->State != EAssetMutationTransactionState::Undone)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"Only an undone asset mutation transaction can be redone.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}
		if (!State->RedoOperation)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"This asset mutation does not support editor-history redo.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}

		FAssetResult Result = State->RedoOperation();
		const bool bRecoveryRequired = State->IsRecoveryRequired
			&& State->IsRecoveryRequired();
		if (Result)
			State->State = EAssetMutationTransactionState::Committed;
		else if (bRecoveryRequired)
			State->State = EAssetMutationTransactionState::RecoveryRequired;
		State->LastResult = {
			.Result = Result,
			.State = State->State,
			.RegistryRevision = FAssetRuntimeState::Get().GetCatalogStore().GetRevision(),
			.bStateRestored = !Result && !bRecoveryRequired,
			.bRecoveryRequired = bRecoveryRequired,
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}
}
