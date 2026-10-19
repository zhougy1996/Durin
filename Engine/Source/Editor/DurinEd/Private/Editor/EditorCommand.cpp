#include "Transactions/Transactor.h"

#include "DObject/ObjectLifecycle.h"

namespace Durin::Editor
{
	auto ApplyEditorChange(DTransactor& Transactor, std::unique_ptr<FCommandChange> Change) -> FTransactorResult
	{
		if (!Change) return {.Code = ETransactorResultCode::Rejected,
			.RejectionCause = FTransactorRejection{.Reason = ETransactorRejectionReason::MissingCustom}};
		FGarbageCollectionDeferralScope DeferCollection;
		if (Change->GetAllocatedSize() > std::numeric_limits<size_t>::max() - sizeof(FCommandChange))
			return {.Code = ETransactorResultCode::Failed, .FailureCause = FTransactorFailure{.Code = ETransactorFailure::EntryAccounting}};
		FScopedTransaction Scope(&Transactor, {"EditorCommand", std::string(Change->GetDescription())});
		if (!Scope.IsOutstanding()) return Transactor.GetLastResult();
		FCommandChangeResult Applied;
		try { Applied = Change->Apply(); }
		catch (const std::exception& Error) { Applied = std::unexpected(FCommandChangeError{Error.what(), true}); }
		catch (...) { Applied = std::unexpected(FCommandChangeError{"Editor command threw an unknown exception.", true}); }
		if (!Applied) return Transactor.ReportChangeFailure(std::move(Change), std::move(Applied.error()));
		const auto Recorded = Transactor.StoreUndo(std::move(Change));
		if (!Recorded) return Recorded;
		(void)Scope.End();
		return Transactor.GetLastResult();
	}
}
