#pragma once

#include "CoreMinimal.h"
#include "RHIContext.h"

namespace Durin
{
	struct FMetalSubmissionState;
	class FMetalCommandContext : public IRHICommandContext
	{
	public:
		virtual auto Configure(std::shared_ptr<FMetalSubmissionState> State) -> void = 0;
		virtual auto CancelPending() -> void = 0;
	};
	auto CreateMetalCommandContext() -> std::unique_ptr<FMetalCommandContext>;
}
