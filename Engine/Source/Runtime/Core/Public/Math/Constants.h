#pragma once

#include <cfloat>

inline constexpr auto PI = 3.1415926535897932f;
inline constexpr auto SMALL_NUMBER = 1.e-8f;
inline constexpr auto KINDA_SMALL_NUMBER = 1.e-4f;
inline constexpr auto BIG_NUMBER = 3.4e+38f;
inline constexpr auto EULERS_NUMBER = 2.71828182845904523536f;
inline constexpr auto GOLDEN_RATIO = 1.6180339887498948482045868343656381f;
inline constexpr auto FLOAT_NON_FRACTIONAL = 8388608.f; // All single-precision floating point numbers greater than or equal to this have no fractional value.

inline constexpr auto DOUBLE_PI = 3.141592653589793238462643383279502884197169399;
inline constexpr auto DOUBLE_SMALL_NUMBER = 1.e-8;
inline constexpr auto DOUBLE_KINDA_SMALL_NUMBER = 1.e-4;
inline constexpr auto DOUBLE_BIG_NUMBER = 3.4e+38;
inline constexpr auto DOUBLE_EULERS_NUMBER = 2.7182818284590452353602874713526624977572;
inline constexpr auto DOUBLE_GOLDEN_RATIO = 1.6180339887498948482045868343656381;
inline constexpr auto DOUBLE_FLOAT_NON_FRACTIONAL = 4503599627370496.0; // All double-precision floating point numbers greater than or equal to this have no fractional value.

inline constexpr auto MAX_FLT = FLT_MAX;
inline constexpr auto MIN_FLT = FLT_MIN;

// Auxiliary constants.
inline constexpr auto INV_PI = 0.31830988618f;
inline constexpr auto TWO_PI = 6.28318530717f;
inline constexpr auto HALF_PI = 1.57079632679f;
inline constexpr auto PI_SQUARED = 9.86960440108f;

inline constexpr auto DOUBLE_INV_PI = 0.31830988618f;
inline constexpr auto DOUBLE_TWO_PI = 6.28318530717f;
inline constexpr auto DOUBLE_HALF_PI = 1.57079632679f;
inline constexpr auto DOUBLE_PI_SQUARED = 9.86960440108f;

// Common square roots
inline constexpr auto SQRT_2 = 1.4142135623730950488016887242097f;
inline constexpr auto SQRT_3 = 1.7320508075688772935274463415059f;
inline constexpr auto INV_SQRT_2 = 0.70710678118654752440084436210485f;
inline constexpr auto INV_SQRT_3 = 0.57735026918962576450914878050196f;
inline constexpr auto HALF_SQRT_2 = 0.70710678118654752440084436210485f;
inline constexpr auto HALF_SQRT_3 = 0.86602540378443864676372317075294f;

inline constexpr auto DOUBLE_SQRT_2 = 1.4142135623730950488016887242097;
inline constexpr auto DOUBLE_SQRT_3 = 1.7320508075688772935274463415059;
inline constexpr auto DOUBLE_INV_SQRT_2 = 0.70710678118654752440084436210485;
inline constexpr auto DOUBLE_INV_SQRT_3 = 0.57735026918962576450914878050196;
inline constexpr auto DOUBLE_HALF_SQRT_2 = 0.70710678118654752440084436210485;
inline constexpr auto DOUBLE_HALF_SQRT_3 = 0.86602540378443864676372317075294;

inline constexpr auto DELTA = 1.e-5f;
inline constexpr auto DOUBLE_DELTA = 1.e-5;