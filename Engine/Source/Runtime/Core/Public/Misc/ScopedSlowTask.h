#pragma once

#include "CoreMinimal.h"
#include "Misc/SlowTask.h"

namespace Durin
{
	// Lexical progress contributes to its parent's current frame. Normal scope exit
	// completes work accounting; exception unwinding fails it. No UI is pumped.
	class FScopedSlowTask final : public FSlowTask
	{
	public:
		CORE_API explicit FScopedSlowTask(double AmountOfWork, std::string Message = {}, FFeedbackContext& Context = FFeedbackContext::Get());
		CORE_API ~FScopedSlowTask() override;
	private:
		int ExceptionsOnEntry = 0;
	};
}
