#pragma once

namespace Doge
{
	/**  A 64-bit hash from XXH3. */
	struct FXxHash64
	{
		uint64 Hash{};

		static auto CORE_API HashBuffer(const void* Data, uint64 Size) -> FXxHash64;
	};

	/** Calculates a 64-bit hash with XXH3. */
	class FXxHash64Builder
	{
	public:
		inline FXxHash64Builder() { Reset(); }

		FXxHash64Builder(const FXxHash64Builder&) = delete;
		FXxHash64Builder& operator=(const FXxHash64Builder&) = delete;

		CORE_API auto Reset() -> void;

		CORE_API auto Update(const void* Data, uint64 Size) -> void;

		[[nodiscard]] CORE_API auto Finalize() const -> FXxHash64;

	private:
		alignas(64) char StateBytes[576];
	};
}