#include "Delegates/Delegate.h"

namespace Durin
{
	static std::atomic<uint32> GNextDelegateHandleId = 0;

	FDelegateHandle::FDelegateHandle()
		: Id(GNextDelegateHandleId++)
	{
	}
} // namespace Durin