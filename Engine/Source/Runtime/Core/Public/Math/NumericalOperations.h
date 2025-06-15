#pragma once

#include <cmath>

#include "HAL/Platform.h"

namespace FMath
{
template<class T>
constexpr FORCEINLINE T Max(const T A, const T B)
{
	return (B < A) ? A : B;
}

template<class T>
constexpr FORCEINLINE T Min(const T A, const T B)
{
	return (A < B) ? A : B;
}

// Abs
FORCEINLINE auto Abs(float Value) -> float { return std::abs(Value); }
FORCEINLINE auto Abs(int32 Value) -> int32 { return std::abs(Value); }
FORCEINLINE auto Abs(int64 Value) -> int64 { return std::abs(Value); }
FORCEINLINE auto Abs(double Value) -> double { return std::abs(Value); }

// Sqrt and InvSqrt
FORCEINLINE auto Sqrt(float Value) -> float { return std::sqrt(Value); }
FORCEINLINE auto Sqrt(double Value) -> double { return std::sqrt(Value); }
FORCEINLINE auto InvSqrt(float Value) -> float { return 1.0f / std::sqrt(Value); }
FORCEINLINE auto InvSqrt(double Value) -> double { return 1.0 / std::sqrt(Value); }

// Trunc
FORCEINLINE auto TruncToFloat(float Value) -> float { return static_cast<float>(std::trunc(Value)); }
FORCEINLINE auto TruncToFloat(double Value) -> float { return static_cast<float>(std::trunc(Value)); }
FORCEINLINE auto TruncToDouble(float Value) -> double { return static_cast<double>(std::trunc(Value)); }
FORCEINLINE auto TruncToDouble(double Value) -> double { return static_cast<double>(std::trunc(Value)); }
FORCEINLINE auto TruncToInt(float Value) -> int32 { return static_cast<int32>(std::trunc(Value)); }
FORCEINLINE auto TruncToInt(double Value) -> int64 { return static_cast<int64>(std::trunc(Value)); }
FORCEINLINE auto TruncToInt32(float Value) -> int32 { return static_cast<int32>(std::trunc(Value)); }
FORCEINLINE auto TruncToInt32(double Value) -> int32 { return static_cast<int32>(std::trunc(Value)); }
FORCEINLINE auto TruncToInt64(float Value) -> int64 { return static_cast<int64>(std::trunc(Value)); }
FORCEINLINE auto TruncToInt64(double Value) -> int64 { return static_cast<int64>(std::trunc(Value)); }

// Floor
FORCEINLINE auto FloorToInt(float Value) -> int32 { return static_cast<int32>(std::floor(Value)); }
FORCEINLINE auto FloorToInt(double Value) -> int64 { return static_cast<int64>(std::floor(Value)); }
FORCEINLINE auto FloorToInt32(float Value) -> int32 { return static_cast<int32>(std::floor(Value)); }
FORCEINLINE auto FloorToInt32(double Value) -> int32 { return static_cast<int32>(std::floor(Value)); }
FORCEINLINE auto FloorToInt64(float Value) -> int64 { return static_cast<int64>(std::floor(Value)); }
FORCEINLINE auto FloorToInt64(double Value) -> int64 { return static_cast<int64>(std::floor(Value)); }

// Round
FORCEINLINE auto RoundToInt(float Value) -> int32 { return static_cast<int32>(std::round(Value)); }
FORCEINLINE auto RoundToInt(double Value) -> int64 { return static_cast<int64>(std::round(Value)); }
FORCEINLINE auto RoundToInt32(float Value) -> int32 { return static_cast<int32>(std::round(Value)); }
FORCEINLINE auto RoundToInt32(double Value) -> int32 { return static_cast<int32>(std::round(Value)); }
FORCEINLINE auto RoundToInt64(float Value) -> int64 { return static_cast<int64>(std::round(Value)); }
FORCEINLINE auto RoundToInt64(double Value) -> int64 { return static_cast<int64>(std::round(Value)); }

// Ceil
FORCEINLINE auto CeilToInt(float Value) -> int32 { return static_cast<int32>(std::ceil(Value)); }
FORCEINLINE auto CeilToInt(double Value) -> int64 { return static_cast<int64>(std::ceil(Value)); }
FORCEINLINE auto CeilToInt32(float Value) -> int32 { return static_cast<int32>(std::ceil(Value)); }
FORCEINLINE auto CeilToInt32(double Value) -> int32 { return static_cast<int32>(std::ceil(Value)); }
FORCEINLINE auto CeilToInt64(float Value) -> int64 { return static_cast<int64>(std::ceil(Value)); }
FORCEINLINE auto CeilToInt64(double Value) -> int64 { return static_cast<int64>(std::ceil(Value)); }

} // namespace FMath