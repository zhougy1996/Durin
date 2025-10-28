#pragma once

#include "HAL/Platform.h"
#include "Templates/IsIntegral.h"

template<typename T>
requires Integral<T>
FORCEINLINE constexpr T Align(T Size, T Alignment)
{
	return (Size + Alignment - 1) & ~(Alignment - 1);
}
