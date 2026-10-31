#pragma once

#include "CoreMinimal.h"
#include "RHIAPI.h"

namespace Durin
{
	// Never returns or unwinds through asset/task recovery. Borrowed inputs only;
	// diagnostics and process exit do not require Core, logging or render admission.
	[[noreturn]] RHI_API auto RHITerminateOnFailure(const char* Operation, const char* Reason, int64 NativeCode = 0) noexcept -> void;

	// Cache capacity is also used for request admission backpressure.
	class FRHIResourceCapacityError : public std::runtime_error
	{
	public:
		explicit FRHIResourceCapacityError(const char* Message) : std::runtime_error(Message) {}
	};
}
