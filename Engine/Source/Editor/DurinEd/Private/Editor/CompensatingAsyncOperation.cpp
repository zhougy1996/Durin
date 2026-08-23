#include "Editor/CompensatingAsyncOperation.h"

namespace Durin::Editor
{
	struct FCompensatingAsyncOperation::FOwnerState
	{
		std::mutex Mutex;
		FCompensatingAsyncOperation* Owner = nullptr;
	};

	FCompensatingAsyncOperation::FCompensatingAsyncOperation()
		: OwnerState(std::make_shared<FOwnerState>())
	{
		OwnerState->Owner = this;
	}

	FCompensatingAsyncOperation::~FCompensatingAsyncOperation()
	{
		DetachOwner();
		if (CancelActive) CancelActive();
		if (Phase == EPhase::Applying && !bRollbackPerformed && Operations.Rollback)
			Operations.Rollback();
	}

	auto FCompensatingAsyncOperation::Begin(FOperations InOperations) -> bool
	{
		if (IsBusy()) return false;
		Reset();
		Operations = std::move(InOperations);
		if (!Operations.Prepare || !Operations.StartApply || !Operations.Commit
			|| !Operations.Rollback || !Operations.StartCompensation)
		{
			SetPrimaryError({}, "The asynchronous operation is missing a required stage.");
			Finish(false);
			return false;
		}

		std::string PrepareError;
		if (!Operations.Prepare(PrepareError))
		{
			SetPrimaryError(PrepareError, "The asynchronous operation could not be prepared.");
			Finish(false);
			return false;
		}
		if (!StartPhase(EPhase::Applying, Operations.StartApply))
		{
			BeginCompensation();
			return false;
		}
		return true;
	}

	auto FCompensatingAsyncOperation::Abort() -> void
	{
		if (!IsBusy()) return;
		if (CancelActive) CancelActive();
		CancelActive = {};
		if (Phase == EPhase::Applying && !bRollbackPerformed)
		{
			Operations.Rollback();
			bRollbackPerformed = true;
		}
		if (Phase == EPhase::Compensating)
			AppendCompensationError({}, "The compensating operation was cancelled.");
		else
			SetPrimaryError({}, "The asynchronous operation was cancelled.");
		Finish(false);
	}

	auto FCompensatingAsyncOperation::Reset() -> void
	{
		if (IsBusy()) return;
		Operations = {};
		CancelActive = {};
		InlineCompletion.reset();
		Error.clear();
		Phase = EPhase::Idle;
		StartingPhase = EPhase::Idle;
		bRollbackPerformed = false;
	}

	auto FCompensatingAsyncOperation::StartPhase(EPhase InPhase, const FStart& Start) -> bool
	{
		Phase = InPhase;
		StartingPhase = InPhase;
		InlineCompletion.reset();
		FAsyncOperationCancel NewCancel;
		std::string StartError;
		const std::shared_ptr<FOwnerState> State = OwnerState;
		const bool bStarted = Start(
			[State, InPhase](bool bSucceeded, std::string CompletionError) {
				FCompensatingAsyncOperation* Owner = nullptr;
				{
					std::lock_guard Lock(State->Mutex);
					Owner = State->Owner;
				}
				if (Owner)
					Owner->ReceiveCompletion(
						InPhase, bSucceeded, std::move(CompletionError));
			},
			NewCancel,
			StartError);
		StartingPhase = EPhase::Idle;
		if (!bStarted)
		{
			if (InPhase == EPhase::Applying)
				SetPrimaryError(
					StartError, "The asynchronous mutation could not be started.");
			else
				AppendCompensationError(
					StartError, "The compensating operation could not be started.");
			return false;
		}
		CancelActive = std::move(NewCancel);
		if (InlineCompletion)
		{
			FCompletionResult Result = std::move(*InlineCompletion);
			InlineCompletion.reset();
			CancelActive = {};
			if (InPhase == EPhase::Applying) CompleteApply(std::move(Result));
			else CompleteCompensation(std::move(Result));
		}
		return true;
	}

	auto FCompensatingAsyncOperation::ReceiveCompletion(
		EPhase CompletedPhase,
		bool bSucceeded,
		std::string InError) -> void
	{
		if (StartingPhase == CompletedPhase)
		{
			InlineCompletion = FCompletionResult{
				.Error = std::move(InError), .bSucceeded = bSucceeded};
			return;
		}
		if (Phase != CompletedPhase) return;
		CancelActive = {};
		FCompletionResult Result{
			.Error = std::move(InError), .bSucceeded = bSucceeded};
		if (CompletedPhase == EPhase::Applying) CompleteApply(std::move(Result));
		else CompleteCompensation(std::move(Result));
	}

	auto FCompensatingAsyncOperation::CompleteApply(FCompletionResult Result) -> void
	{
		if (!Result.bSucceeded)
		{
			SetPrimaryError(Result.Error, "The asynchronous mutation failed.");
			BeginCompensation();
			return;
		}
		std::string CommitError;
		if (!Operations.Commit(CommitError))
		{
			SetPrimaryError(CommitError, "The asynchronous mutation could not be committed.");
			BeginCompensation();
			return;
		}
		Finish(true);
	}

	auto FCompensatingAsyncOperation::CompleteCompensation(FCompletionResult Result) -> void
	{
		if (!Result.bSucceeded)
			AppendCompensationError(
				Result.Error, "The compensating operation did not restore the prior state.");
		Finish(false);
	}

	auto FCompensatingAsyncOperation::BeginCompensation() -> void
	{
		if (!bRollbackPerformed)
		{
			Operations.Rollback();
			bRollbackPerformed = true;
		}
		if (!StartPhase(EPhase::Compensating, Operations.StartCompensation))
		{
			Finish(false);
		}
	}

	auto FCompensatingAsyncOperation::Finish(bool bSucceeded) -> void
	{
		Phase = bSucceeded ? EPhase::Succeeded : EPhase::Failed;
		if (Operations.Finished) Operations.Finished(bSucceeded, Error);
	}

	auto FCompensatingAsyncOperation::SetPrimaryError(
		std::string_view Message,
		std::string_view Fallback) -> void
	{
		if (Error.empty()) Error = Message.empty() ? Fallback : Message;
	}

	auto FCompensatingAsyncOperation::AppendCompensationError(
		std::string_view Message,
		std::string_view Fallback) -> void
	{
		const std::string_view Detail = Message.empty() ? Fallback : Message;
		if (Error.empty()) Error = Detail;
		else Error += std::format(" Compensation also failed: {}", Detail);
	}

	auto FCompensatingAsyncOperation::DetachOwner() -> void
	{
		if (!OwnerState) return;
		std::lock_guard Lock(OwnerState->Mutex);
		OwnerState->Owner = nullptr;
	}
}
