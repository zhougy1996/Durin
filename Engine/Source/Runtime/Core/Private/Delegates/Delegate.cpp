#include "Delegates/Delegate.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextDelegateHandleId = 1;
	}

	auto FDelegateHandle::GenerateNewHandle() -> FDelegateHandle
	{
		uint64 NewId = 0;
		do
		{
			NewId = GNextDelegateHandleId.fetch_add(1, std::memory_order_relaxed);
		}
		while (NewId == 0);
		return FDelegateHandle(NewId);
	}

	namespace Private
	{
		[[noreturn]] auto ReportUnboundDelegateExecution() -> void
		{
			std::terminate();
		}
	}
}
