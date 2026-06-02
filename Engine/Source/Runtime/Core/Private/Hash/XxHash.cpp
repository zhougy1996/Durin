#include "Hash/XxHash.h"
#include "Misc/StringConvert.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace Durin
{
	namespace
	{
		auto BytesToUint64(std::span<const uint8, 8> Bytes) -> uint64
		{
			uint64 Value = 0;
			for (const uint8 Byte : Bytes)
			{
				Value = (Value << 8) | Byte;
			}
			return Value;
		}

		auto Uint64ToBytes(const uint64 Value, std::span<uint8, 8> OutBytes) -> void
		{
			for (size_t Index = 0; Index < OutBytes.size(); ++Index)
			{
				const int Shift = static_cast<int>((OutBytes.size() - 1 - Index) * 8);
				OutBytes[Index] = static_cast<uint8>((Value >> Shift) & 0xff);
			}
		}
	}

	auto FXxHash64::HashBuffer(const void* Data, uint64 Size) -> FXxHash64
	{
		FXxHash64 Result;
		Result.HashValue = XXH3_64bits(Data, Size);
		return Result;
	}

	auto FXxHash64::ToString() const -> std::string
	{
		uint8 Bytes[8] = {};
		Uint64ToBytes(HashValue, Bytes);
		return String::BytesToHex(Bytes);
	}

	auto FXxHash64::FromString(std::string_view Value) -> FXxHash64
	{
		uint8 Bytes[8] = {};
		String::HexToBytes(Value, Bytes);
		return {BytesToUint64(Bytes)};
	}

	auto FXxHash64Builder::Reset() -> void
	{
		static_assert(alignof(FXxHash64Builder) >= alignof(XXH3_state_t), "Adjust FXxHash64Builder alignment to match XXH3_state_t");
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

	auto FXxHash128::HashBuffer(const void* Data, uint64 Size) -> FXxHash128
	{
		const XXH128_hash_t Hash = XXH3_128bits(Data, Size);
		return {Hash.low64, Hash.high64};
	}

	auto FXxHash128::ToString() const -> std::string
	{
		uint8 Bytes[16] = {};
		Uint64ToBytes(HashHigh, std::span<uint8, 8>(Bytes, 8));
		Uint64ToBytes(HashLow, std::span<uint8, 8>(Bytes + 8, 8));
		return String::BytesToHex(Bytes);
	}

	auto FXxHash128::FromString(std::string_view Value) -> FXxHash128
	{
		uint8 Bytes[16] = {};
		String::HexToBytes(Value, Bytes);
		return {BytesToUint64(std::span<const uint8, 8>(Bytes + 8, 8)), BytesToUint64(std::span<const uint8, 8>(Bytes, 8))};
	}

	auto FXxHash128Builder::Reset() -> void
	{
		static_assert(alignof(FXxHash128Builder) >= alignof(XXH3_state_t), "Adjust FXxHash128Builder alignment to match XXH3_state_t");
		static_assert(sizeof(StateBytes) == sizeof(XXH3_state_t), "Adjust the allocation in FXxHash128Builder to match XXH3_state_t");
		XXH3_state_t& State = reinterpret_cast<XXH3_state_t&>(StateBytes);
		XXH3_128bits_reset(&State);
	}

	auto FXxHash128Builder::Update(const void* Data, uint64 Size) -> void
	{
		auto& State = reinterpret_cast<XXH3_state_t&>(StateBytes);
		XXH3_128bits_update(&State, Data, Size);
	}

	auto FXxHash128Builder::Finalize() const -> FXxHash128
	{
		const auto& State = reinterpret_cast<const XXH3_state_t&>(StateBytes);
		const XXH128_hash_t Hash = XXH3_128bits_digest(&State);
		return {Hash.low64, Hash.high64};
	}

} // namespace Durin
