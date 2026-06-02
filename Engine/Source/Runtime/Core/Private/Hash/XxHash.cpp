#include "Hash/XxHash.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace Durin
{
	namespace
	{
		constexpr char GHexDigits[] = "0123456789abcdef";

		auto TryHexCharToNibble(const char Value, uint8& OutNibble) -> bool
		{
			if (Value >= '0' && Value <= '9')
			{
				OutNibble = static_cast<uint8>(Value - '0');
				return true;
			}

			if (Value >= 'a' && Value <= 'f')
			{
				OutNibble = static_cast<uint8>(10 + Value - 'a');
				return true;
			}

			if (Value >= 'A' && Value <= 'F')
			{
				OutNibble = static_cast<uint8>(10 + Value - 'A');
				return true;
			}

			return false;
		}

		auto AppendHexByte(std::string& OutString, uint8 Value) -> void
		{
			OutString.push_back(GHexDigits[(Value >> 4) & 0xf]);
			OutString.push_back(GHexDigits[Value & 0xf]);
		}

		auto AppendHexWord(std::string& OutString, uint64 Value) -> void
		{
			for (int Shift = 56; Shift >= 0; Shift -= 8)
			{
				AppendHexByte(OutString, static_cast<uint8>((Value >> Shift) & 0xff));
			}
		}

		auto BytesToUint64(std::span<const uint8, 8> Bytes) -> uint64
		{
			uint64 Value = 0;
			for (const uint8 Byte : Bytes)
			{
				Value = (Value << 8) | Byte;
			}
			return Value;
		}
	}

	auto BytesToHex(std::span<const uint8> Bytes) -> std::string
	{
		std::string Result;
		Result.reserve(Bytes.size() * 2);
		for (const uint8 Byte : Bytes)
		{
			AppendHexByte(Result, Byte);
		}
		return Result;
	}

	auto HexToBytes(std::string_view Hex, std::span<uint8> OutBytes) -> bool
	{
		if (Hex.size() != OutBytes.size() * 2)
		{
			return false;
		}

		for (size_t Index = 0; Index < OutBytes.size(); ++Index)
		{
			uint8 HighNibble = 0;
			uint8 LowNibble = 0;
			if (!TryHexCharToNibble(Hex[Index * 2], HighNibble) || !TryHexCharToNibble(Hex[Index * 2 + 1], LowNibble))
			{
				return false;
			}

			OutBytes[Index] = static_cast<uint8>((HighNibble << 4) | LowNibble);
		}

		return true;
	}

	auto FXxHash64::HashBuffer(const void* Data, uint64 Size) -> FXxHash64
	{
		FXxHash64 Result;
		Result.HashValue = XXH3_64bits(Data, Size);
		return Result;
	}

	auto FXxHash64::ToString() const -> std::string
	{
		std::string Result;
		Result.reserve(16);
		AppendHexWord(Result, HashValue);
		return Result;
	}

	auto FXxHash64::TryFromString(std::string_view Value, FXxHash64& OutHash) -> bool
	{
		uint8 Bytes[8] = {};
		if (!HexToBytes(Value, Bytes))
		{
			return false;
		}

		OutHash.HashValue = BytesToUint64(Bytes);
		return true;
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
		std::string Result;
		Result.reserve(32);
		AppendHexWord(Result, HashHigh);
		AppendHexWord(Result, HashLow);
		return Result;
	}

	auto FXxHash128::TryFromString(std::string_view Value, FXxHash128& OutHash) -> bool
	{
		uint8 Bytes[16] = {};
		if (!HexToBytes(Value, Bytes))
		{
			return false;
		}

		OutHash.HashHigh = BytesToUint64(std::span<const uint8, 8>(Bytes, 8));
		OutHash.HashLow = BytesToUint64(std::span<const uint8, 8>(Bytes + 8, 8));
		return true;
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
