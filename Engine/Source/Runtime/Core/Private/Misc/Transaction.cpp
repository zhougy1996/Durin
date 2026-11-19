#include "Misc/Transaction.h"

namespace Durin
{
	namespace { thread_local uint64 ReplayingHistoryId = 0; }

	FScopedTransactionHistoryReplay::FScopedTransactionHistoryReplay(uint64 HistoryId)
		: PreviousHistoryId(std::exchange(ReplayingHistoryId, HistoryId)) {}

	FScopedTransactionHistoryReplay::~FScopedTransactionHistoryReplay()
	{
		ReplayingHistoryId = PreviousHistoryId;
	}

	auto FScopedTransactionHistoryReplay::GetHistoryId() -> uint64 { return ReplayingHistoryId; }

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
