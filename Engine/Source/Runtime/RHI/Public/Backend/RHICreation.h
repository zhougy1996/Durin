#pragma once

#include "CoreMinimal.h"
#include "RHIAPI.h"

namespace Durin
{
	// Never returns or unwinds through asset/task recovery. Borrowed inputs only;
	// diagnostics do not require logging or render admission. Launch owns crash capture.
	[[noreturn]] RHI_API auto RHITerminateOnFailure(const char* Operation, const char* Reason, int64 NativeCode = 0) noexcept -> void;

	// Guaranteed nonzero exit without allocating diagnostics or normal teardown.
	[[noreturn]] RHI_API auto RHITerminateOnOutOfMemory(const char* Operation, int64 NativeCode = 0) noexcept -> void;

	// Cache capacity is also used for request admission backpressure.
	class FRHIResourceCapacityError : public std::runtime_error
	{
	public:
		explicit FRHIResourceCapacityError(const char* Message) : std::runtime_error(Message) {}
	};
}
