#include "Misc/Transaction.h"

namespace Durin
{
	namespace { thread_local uint64 ReplayingHistoryId = 0; thread_local std::span<DObject* const> PendingKillObjects; }

	FScopedTransactionHistoryReplay::FScopedTransactionHistoryReplay(uint64 HistoryId)
		: FScopedTransactionHistoryReplay(HistoryId, {}) {}

	FScopedTransactionHistoryReplay::FScopedTransactionHistoryReplay(uint64 HistoryId, std::span<DObject* const> PendingKill)
		: PreviousPendingKill(std::exchange(PendingKillObjects, PendingKill)), PreviousHistoryId(std::exchange(ReplayingHistoryId, HistoryId)) {}

	auto FScopedTransactionHistoryReplay::IncludesPendingKill(const DObject* Object) -> bool
	{ return Object && std::ranges::contains(PendingKillObjects, Object); }

	FScopedTransactionHistoryReplay::~FScopedTransactionHistoryReplay()
	{
		ReplayingHistoryId = PreviousHistoryId;
		PendingKillObjects = PreviousPendingKill;
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
