#pragma once

namespace Doge
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

		DOGE_NONCOPYABLE(FXxHash64Builder);

	private:
		alignas(64) char StateBytes[576];
	};
}

template <>
struct std::hash<Doge::FXxHash64>
{
	auto operator()(const Doge::FXxHash64& Hash) const noexcept -> size_t
	{
		return Hash.HashValue;
	}
};