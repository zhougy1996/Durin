#pragma once

#include "HAL/Platform.h"
#include "Math/Constants.h"
#include "Math/Vector.h"

#include <cmath>
#include <concepts>
#include <numbers>
#include <type_traits>
#include <utility>

namespace Durin::Math
{
	namespace Detail
	{
		template<typename TValue>
		using TUnqualified = std::remove_cvref_t<TValue>;

		template<typename TValue>
		concept CVector = std::same_as<TUnqualified<TValue>, FVector2f>
			|| std::same_as<TUnqualified<TValue>, FVector3f>
			|| std::same_as<TUnqualified<TValue>, FVector4f>
			|| std::same_as<TUnqualified<TValue>, FVector2d>
			|| std::same_as<TUnqualified<TValue>, FVector3d>
			|| std::same_as<TUnqualified<TValue>, FVector4d>;

		template<typename TValue>
		concept CVector3 = std::same_as<TUnqualified<TValue>, FVector3f>
			|| std::same_as<TUnqualified<TValue>, FVector3d>;

		template<typename TValue>
		concept CQuaternion = std::same_as<TUnqualified<TValue>, FQuat>;

		template<typename TValue>
		concept CNormalizable = CVector<TValue> || CQuaternion<TValue>;

		template<typename TValue>
		using TScalar = std::remove_cvref_t<decltype(std::declval<TUnqualified<TValue>>().x)>;

		template<CVector TValue>
		inline constexpr uint32 VectorComponentCount =
			std::same_as<TUnqualified<TValue>, FVector2f> || std::same_as<TUnqualified<TValue>, FVector2d> ? 2u
			: std::same_as<TUnqualified<TValue>, FVector3f> || std::same_as<TUnqualified<TValue>, FVector3d> ? 3u
			: 4u;

		template<CNormalizable TValue>
		[[nodiscard]] constexpr auto ComponentCount() -> uint32
		{
			if constexpr (CQuaternion<TValue>) return 4u;
			else return VectorComponentCount<TValue>;
		}

		template<CNormalizable TValue>
		[[nodiscard]] constexpr auto DefaultMinLengthSquared() -> TScalar<TValue>
		{
			if constexpr (std::same_as<TScalar<TValue>, float>) return kSmallNumber;
			else return kDoubleSmallNumber;
		}
	}

	// Returns true only when every vector or quaternion component is finite.
	template<Detail::CNormalizable TValue>
	[[nodiscard]] FORCEINLINE auto IsFinite(const TValue& Value) -> bool
	{
		for (uint32 Index = 0; Index < Detail::ComponentCount<TValue>(); ++Index)
		{
			if (!std::isfinite(Value[Index])) return false;
		}
		return true;
	}

	// Returns true only when every matrix component is finite. Matrices use [column][row] indexing.
	[[nodiscard]] FORCEINLINE auto IsFinite(const FMatrix& Value) -> bool
	{
		for (uint32 Column = 0; Column < 4; ++Column)
		{
			for (uint32 Row = 0; Row < 4; ++Row)
			{
				if (!std::isfinite(Value[Column][Row])) return false;
			}
		}
		return true;
	}

	template<Detail::CNormalizable TValue>
	[[nodiscard]] FORCEINLINE auto Dot(const TValue& Left, const TValue& Right) -> Detail::TScalar<TValue>
	{
		return glm::dot(Left, Right);
	}

	template<Detail::CNormalizable TValue>
	[[nodiscard]] FORCEINLINE auto LengthSquared(const TValue& Value) -> Detail::TScalar<TValue>
	{
		return Dot(Value, Value);
	}

	template<Detail::CNormalizable TValue>
	[[nodiscard]] FORCEINLINE auto Length(const TValue& Value) -> Detail::TScalar<TValue>
	{
		return glm::length(Value);
	}

	template<Detail::CVector3 TValue>
	[[nodiscard]] FORCEINLINE auto Cross(const TValue& Left, const TValue& Right) -> Detail::TUnqualified<TValue>
	{
		return glm::cross(Left, Right);
	}

	// Requires finite input with non-zero length. Use TryNormalize for authored or otherwise untrusted values.
	template<Detail::CNormalizable TValue>
	[[nodiscard]] FORCEINLINE auto Normalize(const TValue& Value) -> Detail::TUnqualified<TValue>
	{
		return glm::normalize(Value);
	}

	// Leaves OutValue unchanged unless finite input exceeds MinLengthSquared and produces a finite unit value.
	template<Detail::CNormalizable TValue>
	[[nodiscard]] FORCEINLINE auto TryNormalize(
		const TValue& Value,
		Detail::TUnqualified<TValue>& OutValue,
		Detail::TScalar<TValue> MinLengthSquared = Detail::DefaultMinLengthSquared<TValue>()) -> bool
	{
		if (!IsFinite(Value) || !std::isfinite(MinLengthSquared) || MinLengthSquared < 0) return false;
		const Detail::TScalar<TValue> SquaredLength = LengthSquared(Value);
		if (!std::isfinite(SquaredLength) || SquaredLength <= MinLengthSquared) return false;
		const Detail::TUnqualified<TValue> Candidate = Value / std::sqrt(SquaredLength);
		if (!IsFinite(Candidate)) return false;
		OutValue = Candidate;
		return true;
	}

	// Returns Fallback when Value does not satisfy TryNormalize's finite-length contract.
	template<Detail::CNormalizable TValue>
	[[nodiscard]] FORCEINLINE auto NormalizeOr(
		const TValue& Value,
		const Detail::TUnqualified<TValue>& Fallback,
		Detail::TScalar<TValue> MinLengthSquared = Detail::DefaultMinLengthSquared<TValue>())
		-> Detail::TUnqualified<TValue>
	{
		Detail::TUnqualified<TValue> Result;
		return TryNormalize(Value, Result, MinLengthSquared) ? Result : Fallback;
	}

	template<Detail::CVector TValue>
	[[nodiscard]] FORCEINLINE auto Abs(const TValue& Value) -> Detail::TUnqualified<TValue>
	{
		return glm::abs(Value);
	}

	template<Detail::CVector TValue>
	[[nodiscard]] FORCEINLINE auto Min(const TValue& Left, const TValue& Right) -> Detail::TUnqualified<TValue>
	{
		return glm::min(Left, Right);
	}

	template<Detail::CVector TValue>
	[[nodiscard]] FORCEINLINE auto Max(const TValue& Left, const TValue& Right) -> Detail::TUnqualified<TValue>
	{
		return glm::max(Left, Right);
	}

	template<Detail::CVector TValue>
	[[nodiscard]] FORCEINLINE auto Clamp(
		const TValue& Value,
		const TValue& MinValue,
		const TValue& MaxValue) -> Detail::TUnqualified<TValue>
	{
		return glm::clamp(Value, MinValue, MaxValue);
	}

	// Performs an unclamped component-wise interpolation.
	template<Detail::CVector TValue>
	[[nodiscard]] FORCEINLINE auto Lerp(
		const TValue& Start,
		const TValue& End,
		Detail::TScalar<TValue> Alpha) -> Detail::TUnqualified<TValue>
	{
		return glm::mix(Start, End, Alpha);
	}

	template<std::floating_point TScalar>
	[[nodiscard]] constexpr auto Pi() -> TScalar
	{
		return std::numbers::pi_v<TScalar>;
	}

	template<std::floating_point TScalar>
	[[nodiscard]] constexpr auto HalfPi() -> TScalar
	{
		return Pi<TScalar>() / static_cast<TScalar>(2);
	}

	template<std::floating_point TScalar>
	[[nodiscard]] constexpr auto TwoPi() -> TScalar
	{
		return Pi<TScalar>() * static_cast<TScalar>(2);
	}

	[[nodiscard]] FORCEINLINE auto DegreesToRadians(float Degrees) -> float { return glm::radians(Degrees); }
	[[nodiscard]] FORCEINLINE auto DegreesToRadians(double Degrees) -> double { return glm::radians(Degrees); }
	[[nodiscard]] FORCEINLINE auto RadiansToDegrees(float Radians) -> float { return glm::degrees(Radians); }
	[[nodiscard]] FORCEINLINE auto RadiansToDegrees(double Radians) -> double { return glm::degrees(Radians); }

	template<Detail::CVector TValue>
	[[nodiscard]] FORCEINLINE auto DegreesToRadians(const TValue& Degrees) -> Detail::TUnqualified<TValue>
	{
		return glm::radians(Degrees);
	}

	template<Detail::CVector TValue>
	[[nodiscard]] FORCEINLINE auto RadiansToDegrees(const TValue& Radians) -> Detail::TUnqualified<TValue>
	{
		return glm::degrees(Radians);
	}

	// Axis must be finite and normalized. Angle is explicitly expressed in radians.
	[[nodiscard]] FORCEINLINE auto MakeQuaternionFromAxisAngleRadians(FReal Radians, const FVector3& Axis) -> FQuat
	{
		return glm::angleAxis(Radians, Axis);
	}

	// Axis must be finite and normalized. Angle is explicitly expressed in degrees.
	[[nodiscard]] FORCEINLINE auto MakeQuaternionFromAxisAngleDegrees(FReal Degrees, const FVector3& Axis) -> FQuat
	{
		return MakeQuaternionFromAxisAngleRadians(DegreesToRadians(Degrees), Axis);
	}

	[[nodiscard]] FORCEINLINE auto MakeQuaternionFromEulerRadians(const FVector3& Radians) -> FQuat
	{
		return glm::quat(Radians);
	}

	[[nodiscard]] FORCEINLINE auto MakeQuaternionFromEulerDegrees(const FVector3& Degrees) -> FQuat
	{
		return MakeQuaternionFromEulerRadians(DegreesToRadians(Degrees));
	}

	[[nodiscard]] FORCEINLINE auto QuaternionToEulerRadians(const FQuat& Rotation) -> FVector3
	{
		return glm::eulerAngles(Rotation);
	}

	[[nodiscard]] FORCEINLINE auto QuaternionToEulerDegrees(const FQuat& Rotation) -> FVector3
	{
		return RadiansToDegrees(QuaternionToEulerRadians(Rotation));
	}

	[[nodiscard]] FORCEINLINE auto QuaternionFromMatrix(const FMatrix& Matrix) -> FQuat
	{
		return glm::quat_cast(Matrix);
	}

	// Requires a finite, non-zero quaternion. Use TryNormalize before inversion for untrusted values.
	[[nodiscard]] FORCEINLINE auto Inverse(const FQuat& Rotation) -> FQuat
	{
		return glm::inverse(Rotation);
	}

	// Requires a finite, normalized quaternion when rotation without scale is intended.
	[[nodiscard]] FORCEINLINE auto RotateVector(const FQuat& Rotation, const FVector3& Vector) -> FVector3
	{
		return Rotation * Vector;
	}

	// Treats q and -q as the same rotation. Invalid or non-normalizable inputs are never equivalent.
	[[nodiscard]] FORCEINLINE auto AreRotationsEquivalent(
		const FQuat& Left,
		const FQuat& Right,
		FReal Tolerance = kDoubleDelta,
		FReal MinLengthSquared = kDoubleSmallNumber) -> bool
	{
		if (!std::isfinite(Tolerance) || Tolerance < 0.0 || Tolerance > 1.0) return false;
		FQuat NormalizedLeft;
		FQuat NormalizedRight;
		if (!TryNormalize(Left, NormalizedLeft, MinLengthSquared)
			|| !TryNormalize(Right, NormalizedRight, MinLengthSquared)) return false;
		return std::abs(Dot(NormalizedLeft, NormalizedRight)) >= 1.0 - Tolerance;
	}

	[[nodiscard]] FORCEINLINE auto Determinant(const FMatrix& Matrix) -> FReal
	{
		return glm::determinant(Matrix);
	}

	// Requires a finite, nonsingular matrix. Use TryInverse for authored or otherwise untrusted values.
	[[nodiscard]] FORCEINLINE auto Inverse(const FMatrix& Matrix) -> FMatrix
	{
		return glm::inverse(Matrix);
	}

	// Leaves OutInverse unchanged unless Matrix has a finite determinant above MinAbsDeterminant and a finite inverse.
	[[nodiscard]] FORCEINLINE auto TryInverse(
		const FMatrix& Matrix,
		FMatrix& OutInverse,
		FReal MinAbsDeterminant = kDoubleSmallNumber) -> bool
	{
		if (!IsFinite(Matrix) || !std::isfinite(MinAbsDeterminant) || MinAbsDeterminant < 0.0) return false;
		const FReal MatrixDeterminant = Determinant(Matrix);
		if (!std::isfinite(MatrixDeterminant) || std::abs(MatrixDeterminant) <= MinAbsDeterminant) return false;
		const FMatrix Candidate = Inverse(Matrix);
		if (!IsFinite(Candidate)) return false;
		OutInverse = Candidate;
		return true;
	}

	[[nodiscard]] FORCEINLINE auto Transpose(const FMatrix& Matrix) -> FMatrix
	{
		return glm::transpose(Matrix);
	}

	[[nodiscard]] FORCEINLINE auto TranslationMatrix(const FVector3& Translation) -> FMatrix
	{
		return glm::translate(FMatrix(1.0), Translation);
	}

	[[nodiscard]] FORCEINLINE auto ScaleMatrix(const FVector3& Scale) -> FMatrix
	{
		return glm::scale(FMatrix(1.0), Scale);
	}

	[[nodiscard]] FORCEINLINE auto RotationMatrix(const FQuat& Rotation) -> FMatrix
	{
		return glm::mat4_cast(Rotation);
	}
}
