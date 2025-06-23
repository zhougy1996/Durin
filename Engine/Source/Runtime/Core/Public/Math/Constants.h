#pragma once

#include <cfloat>

inline constexpr auto kPi = 3.1415926535897932f;
inline constexpr auto kSmallNumber = 1.e-8f;
inline constexpr auto kKindaSmallNumber = 1.e-4f;
inline constexpr auto kBigNumber = 3.4e+38f;
inline constexpr auto kEulersNumber = 2.71828182845904523536f;
inline constexpr auto kGoldenRatio = 1.6180339887498948482045868343656381f;
inline constexpr auto kFloatNonFractional = 8388608.f; // All single-precision floating point numbers greater than or equal to this have no fractional value.

inline constexpr auto kDoublePi = 3.141592653589793238462643383279502884197169399;
inline constexpr auto kDoubleSmallNumber = 1.e-8;
inline constexpr auto kDoubleKindaSmallNumber = 1.e-4;
inline constexpr auto kDoubleBigNumber = 3.4e+38;
inline constexpr auto kDoubleEulersNumber = 2.7182818284590452353602874713526624977572;
inline constexpr auto kDoubleGoldenRatio = 1.6180339887498948482045868343656381;
inline constexpr auto kDoubleFloatNonFractional = 4503599627370496.0; // All double-precision floating point numbers greater than or equal to this have no fractional value.

inline constexpr auto kMaxFloat = FLT_MAX;
inline constexpr auto kMinFloat = FLT_MIN;

// Auxiliary constants.
inline constexpr auto kInvPi = 0.31830988618f;
inline constexpr auto kTwoPi = 6.28318530717f;
inline constexpr auto kHalfPi = 1.57079632679f;
inline constexpr auto kPiSquared = 9.86960440108f;

inline constexpr auto kDoubleInvPi = 0.31830988618f;
inline constexpr auto kDoubleTwoPi = 6.28318530717f;
inline constexpr auto kDoubleHalfPi = 1.57079632679f;
inline constexpr auto kDoublePiSquared = 9.86960440108f;

// Common square roots
inline constexpr auto kSqrt2 = 1.4142135623730950488016887242097f;
inline constexpr auto kSqrt3 = 1.7320508075688772935274463415059f;
inline constexpr auto kInvSqrt2 = 0.70710678118654752440084436210485f;
inline constexpr auto kInvSqrt3 = 0.57735026918962576450914878050196f;
inline constexpr auto kHalfSqrt2 = 0.70710678118654752440084436210485f;
inline constexpr auto kHalfSqrt3 = 0.86602540378443864676372317075294f;

inline constexpr auto kDoubleSqrt2 = 1.4142135623730950488016887242097;
inline constexpr auto kDoubleSqrt3 = 1.7320508075688772935274463415059;
inline constexpr auto kDoubleInvSqrt2 = 0.70710678118654752440084436210485;
inline constexpr auto kDoubleInvSqrt3 = 0.57735026918962576450914878050196;
inline constexpr auto kDoubleHalfSqrt2 = 0.70710678118654752440084436210485;
inline constexpr auto kDoubleHalfSqrt3 = 0.86602540378443864676372317075294;

inline constexpr auto kDelta = 1.e-5f;
inline constexpr auto kDoubleDelta = 1.e-5;