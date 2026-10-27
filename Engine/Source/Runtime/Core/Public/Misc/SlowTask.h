#pragma once

#include "CoreMinimal.h"
#include "Misc/FeedbackContext.h"

namespace Durin
{
	struct FSlowTaskData;

	// A feedback lifetime, including across ordinary host frames. Zero work is
	// indeterminate. Destruction removes feedback; it does not report business success.
	// Only lexical FScopedSlowTask instances become implicit parents on their thread.
	class FSlowTask
	{
	public:
		CORE_API explicit FSlowTask(double AmountOfWork, std::string Message = {}, FFeedbackContext& Context = FFeedbackContext::Get());
		CORE_API virtual ~FSlowTask();
		FSlowTask(const FSlowTask&) = delete;
		auto operator=(const FSlowTask&) -> FSlowTask& = delete;
		// Closes the previous frame and reserves this frame's share of total work.
		CORE_API auto EnterProgressFrame(double ExpectedWork = 1, std::string Message = {}) -> void;
		// Adapts externally measured progress; nullopt explicitly means unknown.
		CORE_API auto SetProgress(std::optional<double> Fraction, std::string Message = {}) -> void;
		CORE_API auto SetMessage(std::string Message) -> void;
		CORE_API auto SetDetails(std::string Details) -> void;
		CORE_API auto MakeDialog(bool bShowCancelButton = false) -> void;
		CORE_API auto MakeDialogDelayed(double ThresholdSeconds, bool bShowCancelButton = false) -> void;
		CORE_API auto ShouldCancel() const -> bool;
		CORE_API auto Complete() -> void;
		CORE_API auto Fail(std::string Diagnostic) -> void;
		CORE_API auto Cancel() -> void;
		CORE_API auto GetSnapshot() const -> std::optional<FSlowTaskSnapshot>;
	protected:
		CORE_API auto PushScope() -> void;
	private:
		auto Finish(ESlowTaskStatus Status, std::string Diagnostic = {}) -> void;
		std::shared_ptr<FSlowTaskContextState> State;
		std::shared_ptr<FSlowTaskData> Data;
	};
}
