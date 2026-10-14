#pragma once

#include "CoreMinimal.h"

namespace Durin::DerivedData
{
	enum class ERequestWaitResult : uint8 { Completed, WouldBlock, InvalidRequest };
	enum class ERequestPriority : uint8 { High, Normal, Low };
	class IRequest
	{
	public:
		virtual ~IRequest() = default;
		// Nonblocking signal for cancellation propagation and callbacks.
		virtual auto RequestCancel() -> bool = 0;
		// Cancel and Wait wait for request end, including callback exit.
		// Cancel rejects self-waits.
		virtual auto Cancel() -> bool = 0;
		virtual auto Wait() -> ERequestWaitResult = 0;
	};
}
