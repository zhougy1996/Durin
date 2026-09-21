#pragma once

#include "DurinEdAPI.h"

namespace Durin::Editor
{
	using FAsyncOperationCompletion = std::function<void(bool, std::string)>;
	using FAsyncOperationCancel = std::function<void()>;

	// Coordinates an asynchronous mutation whose prepared side effects must be
	// committed on success or rolled back and asynchronously restored on failure.
	class FCompensatingAsyncOperation final
	{
	public:
		enum class EPhase : uint8
		{
			Idle,
			Applying,
			Compensating,
			Succeeded,
			Failed
		};

		using FStart = std::function<bool(
			FAsyncOperationCompletion,
			FAsyncOperationCancel&,
			std::string&)>;

		struct FOperations
		{
			std::function<bool(std::string&)> Prepare;
			FStart StartApply;
			std::function<bool(std::string&)> Commit;
			std::function<void()> Rollback;
			FStart StartCompensation;
			std::function<void(bool, std::string_view)> Finished;
		};

		DURINED_API FCompensatingAsyncOperation();
		DURINED_API ~FCompensatingAsyncOperation();
		FCompensatingAsyncOperation(const FCompensatingAsyncOperation&) = delete;
		auto operator=(const FCompensatingAsyncOperation&)
			-> FCompensatingAsyncOperation& = delete;

		// Completion callbacks supplied to StartApply and StartCompensation must
		// execute on the owning thread. Inline completion is supported.
		DURINED_API auto Begin(FOperations InOperations) -> bool;
		DURINED_API auto Abort() -> void;
		DURINED_API auto Reset() -> void;

		auto GetPhase() const -> EPhase { return Phase; }
		auto GetError() const -> const std::string& { return Error; }
		auto IsBusy() const -> bool
		{
			return Phase == EPhase::Applying || Phase == EPhase::Compensating;
		}

	private:
		struct FOwnerState;
		struct FCompletionResult
		{
			std::string Error;
			bool bSucceeded = false;
		};

		auto StartPhase(EPhase InPhase, const FStart& Start) -> bool;
		auto ReceiveCompletion(EPhase CompletedPhase, bool bSucceeded, std::string InError) -> void;
		auto CompleteApply(FCompletionResult Result) -> void;
		auto CompleteCompensation(FCompletionResult Result) -> void;
		auto BeginCompensation() -> void;
		auto Finish(bool bSucceeded) -> void;
		auto SetPrimaryError(std::string_view Message, std::string_view Fallback) -> void;
		auto AppendCompensationError(std::string_view Message, std::string_view Fallback) -> void;
		auto DetachOwner() -> void;

		std::shared_ptr<FOwnerState> OwnerState;
		FOperations Operations;
		FAsyncOperationCancel CancelActive;
		std::optional<FCompletionResult> InlineCompletion;
		std::string Error;
		EPhase Phase = EPhase::Idle;
		EPhase StartingPhase = EPhase::Idle;
		bool bRollbackPerformed = false;
	};
}
