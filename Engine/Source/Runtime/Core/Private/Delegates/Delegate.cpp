#include "Delegates/Delegate.h"

namespace Doge
{
	static std::atomic<uint32> GNextDelegateHandleId = 0;

	FDelegateHandle::FDelegateHandle()
		: Id(GNextDelegateHandleId++)
	{
	}
} // namespace Doge