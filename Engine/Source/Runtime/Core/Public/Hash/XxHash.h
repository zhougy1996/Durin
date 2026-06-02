#pragma once

#include "CoreAPI.h"
#include "Misc/AssertionMacros.h"
#include "Misc/CoreMiscDefines.h"
#include "Misc/CoreTypes.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace Durin
{
	[[nodiscard]] CORE_API auto BytesToHex(std::span<const uint8> Bytes) -> std::string;

	[[nodiscard]] FORCEINLINE auto BytesToHex(std::span<const std::byte> Bytes) -> std::string
	{
		return BytesToHex(std::span<const uint8>(reinterpret_cast<const uint8*>(Bytes.data()), Bytes.size()));
	}

	CORE_API auto HexToBytes(std::string_view Hex, std::span<uint8> OutBytes) -> bool;

	/**  A 64-bit hash from XXH3. */
	struct FXxHash64
	{
		uint64 HashValue{};

		[[nodiscard]] static auto CORE_API HashBuffer(const void* Data, uint64 Size) -> FXxHash64;

		[[nodiscard]] FORCEINLINE static auto HashBuffer(std::span<const std::byte> Bytes) -> FXxHash64
		{
			return HashBuffer(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		[[nodiscard]] FORCEINLINE static auto HashBuffer(std::span<const uint8> Bytes) -> FXxHash64
		{
			return HashBuffer(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		[[nodiscard]] FORCEINLINE static auto HashBuffer(std::string_view Value) -> FXxHash64
		{
			return HashBuffer(Value.empty() ? nullptr : Value.data(), static_cast<uint64>(Value.size()));
		}

		[[nodiscard]] CORE_API auto ToString() const -> std::string;

		[[nodiscard]] static auto CORE_API TryFromString(std::string_view Value, FXxHash64& OutHash) -> bool;

		[[nodiscard]] FORCEINLINE static auto FromString(std::string_view Value) -> FXxHash64
		{
			FXxHash64 Result;
			check(TryFromString(Value, Result));
			return Result;
		}

		FORCEINLINE auto operator==(const FXxHash64& Other) const -> bool
		{
			return HashValue == Other.HashValue;
		}

		FORCEINLINE auto operator!=(const FXxHash64& Other) const -> bool
		{
			return HashValue != Other.HashValue;
		}

		FORCEINLINE auto operator<(const FXxHash64& Other) const -> bool
		{
			return HashValue < Other.HashValue;
		}

		FORCEINLINE auto IsZero() const -> bool
		{
			return HashValue == 0;
		}
	};

	/** Calculates a 64-bit hash with XXH3. */
	class FXxHash64Builder
	{
	public:
		FORCEINLINE FXxHash64Builder() { Reset(); }

		CORE_API auto Reset() -> void;

		CORE_API auto Update(const void* Data, uint64 Size) -> void;

		FORCEINLINE auto Update(std::span<const std::byte> Bytes) -> void
		{
			Update(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		FORCEINLINE auto Update(std::span<const uint8> Bytes) -> void
		{
			Update(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		FORCEINLINE auto Update(std::string_view Value) -> void
		{
			Update(Value.empty() ? nullptr : Value.data(), static_cast<uint64>(Value.size()));
		}

		template<typename TValue>
			requires std::is_trivially_copyable_v<TValue>
		FORCEINLINE auto UpdateValue(const TValue& Value) -> void
		{
			Update(&Value, sizeof(Value));
		}

		[[nodiscard]] CORE_API auto Finalize() const -> FXxHash64;

		DURIN_NONCOPYABLE(FXxHash64Builder);

	private:
		static constexpr size_t StateSize = 576;

		alignas(64) char StateBytes[StateSize];
	};

	/** A 128-bit hash from XXH3. */
	struct FXxHash128
	{
		union
		{
			uint64 HashValue[2]{};
			struct
			{
				uint64 HashLow;
				uint64 HashHigh;
			};
		};

		[[nodiscard]] static auto CORE_API HashBuffer(const void* Data, uint64 Size) -> FXxHash128;

		[[nodiscard]] FORCEINLINE static auto HashBuffer(std::span<const std::byte> Bytes) -> FXxHash128
		{
			return HashBuffer(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		[[nodiscard]] FORCEINLINE static auto HashBuffer(std::span<const uint8> Bytes) -> FXxHash128
		{
			return HashBuffer(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		[[nodiscard]] FORCEINLINE static auto HashBuffer(std::string_view Value) -> FXxHash128
		{
			return HashBuffer(Value.empty() ? nullptr : Value.data(), static_cast<uint64>(Value.size()));
		}

		[[nodiscard]] CORE_API auto ToString() const -> std::string;

		[[nodiscard]] static auto CORE_API TryFromString(std::string_view Value, FXxHash128& OutHash) -> bool;

		[[nodiscard]] FORCEINLINE static auto FromString(std::string_view Value) -> FXxHash128
		{
			FXxHash128 Result;
			check(TryFromString(Value, Result));
			return Result;
		}

		FORCEINLINE auto operator==(const FXxHash128& Other) const -> bool
		{
			return HashLow == Other.HashLow && HashHigh == Other.HashHigh;
		}

		FORCEINLINE auto operator!=(const FXxHash128& Other) const -> bool
		{
			return !(*this == Other);
		}

		FORCEINLINE auto operator<(const FXxHash128& Other) const -> bool
		{
			return (HashHigh < Other.HashHigh) || (HashHigh == Other.HashHigh && HashLow < Other.HashLow);
		}

		FORCEINLINE auto IsZero() const -> bool
		{
			return HashLow == 0 && HashHigh == 0;
		}
	};

	class FXxHash128Builder
	{
	public:
		FORCEINLINE FXxHash128Builder() { Reset(); }

		CORE_API auto Reset() -> void;

		CORE_API auto Update(const void* Data, uint64 Size) -> void;

		FORCEINLINE auto Update(std::span<const std::byte> Bytes) -> void
		{
			Update(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		FORCEINLINE auto Update(std::span<const uint8> Bytes) -> void
		{
			Update(Bytes.empty() ? nullptr : Bytes.data(), static_cast<uint64>(Bytes.size_bytes()));
		}

		FORCEINLINE auto Update(std::string_view Value) -> void
		{
			Update(Value.empty() ? nullptr : Value.data(), static_cast<uint64>(Value.size()));
		}

		template<typename TValue>
			requires std::is_trivially_copyable_v<TValue>
		FORCEINLINE auto UpdateValue(const TValue& Value) -> void
		{
			Update(&Value, sizeof(Value));
		}

		[[nodiscard]] CORE_API auto Finalize() const -> FXxHash128;

		DURIN_NONCOPYABLE(FXxHash128Builder);

	private:
		static constexpr size_t StateSize = 576;

		alignas(64) char StateBytes[StateSize];
	};

} // namespace Durin

template<>
struct std::hash<Durin::FXxHash64>
{
	auto operator()(const Durin::FXxHash64& Hash) const noexcept -> size_t
	{
		return Hash.HashValue;
	}
};

template<>
struct std::hash<Durin::FXxHash128>
{
	auto operator()(const Durin::FXxHash128& Hash) const noexcept -> size_t
	{
		const Durin::uint64 Mixed = Hash.HashLow ^ (Hash.HashHigh + 0x9e3779b97f4a7c15ull + (Hash.HashLow << 6) + (Hash.HashLow >> 2));
		return static_cast<size_t>(Mixed);
	}
};
