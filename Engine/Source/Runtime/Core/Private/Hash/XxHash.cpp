#include "Hash/XxHash.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace Doge
{
	auto FXxHash64::HashBuffer(const void* Data, uint64 Size) -> FXxHash64
	{
		FXxHash64 Result;
		Result.Hash = XXH3_64bits(Data, Size);
		return Result;
	}

	auto FXxHash64Builder::Reset() -> void
	{
		static_assert(sizeof(StateBytes) == sizeof(XXH3_state_t), "Adjust the allocation in FXxHash64Builder to match XXH3_state_t");
		XXH3_state_t& State = reinterpret_cast<XXH3_state_t&>(StateBytes);
		XXH3_64bits_reset(&State);
	}

	auto FXxHash64Builder::Update(const void* Data, const uint64 Size) -> void
	{
		auto& State = reinterpret_cast<XXH3_state_t&>(StateBytes);
		XXH3_64bits_update(&State, Data, Size);
	}

	auto FXxHash64Builder::Finalize() const -> FXxHash64
	{
		const auto& State = reinterpret_cast<const XXH3_state_t&>(StateBytes);
		const XXH64_hash_t Hash = XXH3_64bits_digest(&State);
		return {Hash};
	}
} // namespace Doge