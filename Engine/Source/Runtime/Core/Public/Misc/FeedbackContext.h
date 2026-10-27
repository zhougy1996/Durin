#pragma once

#include "CoreMinimal.h"
#include "CoreAPI.h"

namespace Durin
{
	struct FSlowTaskContextState;
	class FSlowTask;
	class FScopedSlowTask;

	// Terminal results are distinct from a cooperative cancellation request.
	enum class ESlowTaskStatus : uint8 { Running, Completed, Failed, Canceled };

	// Detached presentation data; no callbacks, UI pointers, or task ownership escape.
	struct FSlowTaskSnapshot
	{
		uint64 Id = 0;
		std::string Title, Message, Details, Diagnostic;
		std::optional<double> Fraction;
		double ElapsedSeconds = 0;
		ESlowTaskStatus Status = ESlowTaskStatus::Running;
		bool bDialogVisible = false, bCanCancel = false, bCancelRequested = false;
	};

	// Owns feedback admission and per-thread lexical scope stacks, never a work/event pump.
	// Updates, snapshots, and cancellation are serialized. Retiring the context invalidates
	// surviving task handles. Clock must be monotonic and must not reenter this context.
	class FFeedbackContext
	{
	public:
		CORE_API explicit FFeedbackContext(std::function<double()> Clock = {});
		CORE_API ~FFeedbackContext();
		FFeedbackContext(const FFeedbackContext&) = delete;
		auto operator=(const FFeedbackContext&) -> FFeedbackContext& = delete;
		CORE_API static auto Get() -> FFeedbackContext&;
		CORE_API auto GetVisibleTask() const -> std::optional<FSlowTaskSnapshot>;
		CORE_API auto RequestCancel(uint64 Id) -> bool;
		CORE_API auto Retire() -> void;
	private:
		friend class FSlowTask;
		std::shared_ptr<FSlowTaskContextState> State;
	};
}
