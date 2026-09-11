#pragma once

#include <cfloat>

namespace Durin
{
	inline constexpr auto Pi = 3.1415926535897932f;
	inline constexpr auto SmallNumber = 1.e-8f;
	inline constexpr auto KindaSmallNumber = 1.e-4f;
	inline constexpr auto BigNumber = 3.4e+38f;
	inline constexpr auto EulersNumber = 2.71828182845904523536f;
	inline constexpr auto GoldenRatio = 1.6180339887498948482045868343656381f;
	inline constexpr auto FloatNonFractional = 8388608.f; // All single-precision floating point numbers greater than or equal to this have no fractional value.

	inline constexpr auto DoublePi = 3.141592653589793238462643383279502884197169399;
	inline constexpr auto DoubleSmallNumber = 1.e-8;
	inline constexpr auto DoubleKindaSmallNumber = 1.e-4;
	inline constexpr auto DoubleBigNumber = 3.4e+38;
	inline constexpr auto DoubleEulersNumber = 2.7182818284590452353602874713526624977572;
	inline constexpr auto DoubleGoldenRatio = 1.6180339887498948482045868343656381;
	inline constexpr auto DoubleFloatNonFractional = 4503599627370496.0; // All double-precision floating point numbers greater than or equal to this have no fractional value.

	inline constexpr auto MaxFloat = FLT_MAX;
	inline constexpr auto MinFloat = FLT_MIN;

	// Auxiliary constants.
	inline constexpr auto InvPi = 0.31830988618f;
	inline constexpr auto TwoPi = 6.28318530717f;
	inline constexpr auto HalfPi = 1.57079632679f;
	inline constexpr auto PiSquared = 9.86960440108f;

	inline constexpr auto DoubleInvPi = 0.318309886183790671537767526745028724;
	inline constexpr auto DoubleTwoPi = 6.283185307179586476925286766559005768;
	inline constexpr auto DoubleHalfPi = 1.570796326794896619231321691639751442;
	inline constexpr auto DoublePiSquared = 9.869604401089358618834490999876151135;

	// Common square roots
	inline constexpr auto Sqrt2 = 1.4142135623730950488016887242097f;
	inline constexpr auto Sqrt3 = 1.7320508075688772935274463415059f;
	inline constexpr auto InvSqrt2 = 0.70710678118654752440084436210485f;
	inline constexpr auto InvSqrt3 = 0.57735026918962576450914878050196f;
	inline constexpr auto HalfSqrt2 = 0.70710678118654752440084436210485f;
	inline constexpr auto HalfSqrt3 = 0.86602540378443864676372317075294f;

	inline constexpr auto DoubleSqrt2 = 1.4142135623730950488016887242097;
	inline constexpr auto DoubleSqrt3 = 1.7320508075688772935274463415059;
	inline constexpr auto DoubleInvSqrt2 = 0.70710678118654752440084436210485;
	inline constexpr auto DoubleInvSqrt3 = 0.57735026918962576450914878050196;
	inline constexpr auto DoubleHalfSqrt2 = 0.70710678118654752440084436210485;
	inline constexpr auto DoubleHalfSqrt3 = 0.86602540378443864676372317075294;

	// General comparison tolerances, not machine epsilon.
	inline constexpr auto Epsilon = 1.e-5f;
	inline constexpr auto DoubleEpsilon = 1.e-5;
}
