#pragma once

#include "DObject/WeakObjectPtr.h"

#include <algorithm>
#include <list>
#include <unordered_map>

namespace Durin::Private
{
	inline constexpr uint32 MaterialCompileMaxRetryChecks = 256;

	// Game-thread only. Handles do not keep materials alive; generation is part
	// of the key so a reused object slot never inherits an old registration.
	class FMaterialCompileRetryQueue
	{
	public:
		FMaterialCompileRetryQueue() = default;
		FMaterialCompileRetryQueue(const FMaterialCompileRetryQueue&) = delete;
		auto operator=(const FMaterialCompileRetryQueue&) -> FMaterialCompileRetryQueue& = delete;

		auto Add(FWeakObjectPtr Owner) -> void
		{
			if (Owner.GetKey().IsNull() || Entries.contains(Owner.GetKey())) return;
			Owners.push_back(Owner);
			Entries.emplace(Owner.GetKey(), std::prev(Owners.end()));
		}

		auto Remove(FWeakObjectPtr Owner) -> void
		{
			const auto It = Entries.find(Owner.GetKey());
			if (It == Entries.end()) return;
			Owners.erase(It->second);
			Entries.erase(It);
		}

		auto Num() const -> size_t { return Owners.size(); }

		// Every visited entry costs budget, including stale or not-yet-due ones.
		// Pop before calling out, since submission/cancellation may mutate the queue.
		template <typename Visitor>
		auto Process(size_t MaximumChecks, Visitor&& Visit) -> void
		{
			const size_t Count = std::min(MaximumChecks, Num());
			for (size_t Index = 0; Index < Count && !Owners.empty(); ++Index)
			{
				const FWeakObjectPtr Owner = Owners.front();
				Remove(Owner);
				if (Visit(Owner)) Add(Owner);
			}
		}

	private:
std::list<FWeakObjectPtr> Owners;
		std::unordered_map<FObjectKey, std::list<FWeakObjectPtr>::iterator> Entries;
	};

	// Shared by lifecycle and owner teardown, including edits before manager start.
	auto GetMaterialCompileRetryQueue() -> FMaterialCompileRetryQueue&;
}
