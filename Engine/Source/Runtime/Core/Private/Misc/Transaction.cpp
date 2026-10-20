#include "Misc/Transaction.h"

namespace Durin
{
	auto AllocateTransactionOperationId() -> uint64
	{
		static std::atomic<uint64> NextId{1};
		uint64 Candidate = NextId.load();
		while (Candidate != 0)
		{
			const uint64 Next = Candidate == std::numeric_limits<uint64>::max() ? 0 : Candidate + 1;
			if (NextId.compare_exchange_weak(Candidate, Next)) return Candidate;
		}
		return 0;
	}
}
