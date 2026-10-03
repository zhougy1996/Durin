#pragma once

#include "Misc/CoreStd.h"

#include "Misc/CoreTypes.h"

namespace Durin
{
	enum class EBinaryByteOrder : uint8 { LittleEndian, BigEndian };

	template<typename T>
	concept CBinaryInteger = std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>;

	// Fixed-width bytes without an Archive or allocation; signed integers use modulo representation.
	template<CBinaryInteger T>
	constexpr auto EncodeBinaryInteger(T Value, EBinaryByteOrder Order = EBinaryByteOrder::LittleEndian)
		-> std::array<std::byte, sizeof(T)>
	{
		const auto Encoded = static_cast<std::make_unsigned_t<T>>(Value);
		std::array<std::byte, sizeof(T)> Bytes{};
		for (size_t Index = 0; Index < sizeof(T); ++Index)
		{
			const size_t Shift = Order == EBinaryByteOrder::LittleEndian ? Index : sizeof(T) - Index - 1;
			Bytes[Index] = static_cast<std::byte>(Encoded >> (Shift * 8));
		}
		return Bytes;
	}
}
