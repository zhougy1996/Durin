#pragma once

#include "CoreMinimal.h"
#include "Templates/RefCounting.h"

namespace Durin::DerivedData
{
	enum class ERequestPriority : uint8 { High, Normal, Low };
	class IRequest
	{
	public:
		virtual ~IRequest() = default;
		auto AddRef() -> void { References.fetch_add(1, std::memory_order_relaxed); }
		auto Release() -> void { if (References.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this; }
		// Cancel and Wait wait for request end, including callback exit.
		// Waiting or canceling from the request callback is a programming error.
		virtual auto Cancel() -> void = 0;
		virtual auto Wait() -> void = 0;
		virtual auto SetPriority(ERequestPriority Priority) -> void = 0;
	private:
		std::atomic<uint32> References = 0;
	};
}
