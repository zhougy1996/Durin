#pragma once

#include "CoreAPI.h"

namespace Durin
{
	/**  A 64-bit hash from XXH3. */
	struct FXxHash64
	{
		size_t HashValue{};

		static auto CORE_API HashBuffer(const void* Data, uint64 Size) -> FXxHash64;

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

		[[nodiscard]] CORE_API auto Finalize() const -> FXxHash64;

		DURIN_NONCOPYABLE(FXxHash64Builder);

	private:
		alignas(64) char StateBytes[576];
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

		static auto CORE_API HashBuffer(const void* Data, uint64 Size) -> FXxHash128;

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

		[[nodiscard]] CORE_API auto Finalize() const -> FXxHash128;

		DURIN_NONCOPYABLE(FXxHash128Builder);

	private:
		alignas(64) char StateBytes[576];

	};
}

template <>
struct std::hash<Durin::FXxHash64>
{
	auto operator()(const Durin::FXxHash64& Hash) const noexcept -> size_t
	{
		return Hash.HashValue;
	}
};

template <>
struct std::hash<Durin::FXxHash128>
{
	auto operator()(const Durin::FXxHash128& Hash) const noexcept -> size_t
	{
		return Hash.HashLow;
	}
};