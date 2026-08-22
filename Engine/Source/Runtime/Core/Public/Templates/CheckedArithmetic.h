#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	// Checked unsigned arithmetic leaves its output unchanged on invalid input or bound failure.
	[[nodiscard]] inline auto TryAdd(uint64 Left, uint64 Right, uint64 Maximum, uint64& OutValue) -> bool
	{
		if (Right > Maximum || Left > Maximum - Right) return false;
		OutValue = Left + Right;
		return true;
	}

	[[nodiscard]] inline auto TryMultiply(uint64 Left, uint64 Right, uint64 Maximum, uint64& OutValue) -> bool
	{
		if (Left != 0 && Right > Maximum / Left) return false;
		OutValue = Left * Right;
		return true;
	}

	[[nodiscard]] inline auto IsPowerOfTwo(uint64 Value) -> bool
	{
		return Value != 0 && (Value & (Value - 1)) == 0;
	}

	[[nodiscard]] inline auto TryAlignUp(
		uint64 Value,
		uint64 Alignment,
		uint64 Maximum,
		uint64& OutValue) -> bool
	{
		if (!IsPowerOfTwo(Alignment)) return false;
		const uint64 Mask = Alignment - 1;
		if (Value > std::numeric_limits<uint64>::max() - Mask) return false;
		const uint64 Candidate = (Value + Mask) & ~Mask;
		if (Candidate > Maximum) return false;
		OutValue = Candidate;
		return true;
	}

	[[nodiscard]] inline auto TryNarrowSize(uint64 Value, size_t& OutValue) -> bool
	{
		if (Value > std::numeric_limits<size_t>::max()) return false;
		OutValue = static_cast<size_t>(Value);
		return true;
	}
}
