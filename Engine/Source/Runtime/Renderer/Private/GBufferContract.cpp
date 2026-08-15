#include "GBufferContract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Durin::GBufferContract
{
	namespace
	{
		auto SafeNormal(const FVector3f& Value) -> FVector3f
		{
			const float LengthSquared = Math::Dot(Value, Value);
			return std::isfinite(LengthSquared) && LengthSquared > 1.0e-8f
				? Value / std::sqrt(LengthSquared)
				: FVector3f(0.0f, 0.0f, 1.0f);
		}

		auto Determinant(const std::array<FVector3, 3>& Rows) -> double
		{
			return Math::Dot(Rows[0], Math::Cross(Rows[1], Rows[2]));
		}
	} // namespace

	auto EncodeOctahedralNormal(const FVector3f& Normal) -> FVector2f
	{
		FVector3f Projected = SafeNormal(Normal);
		Projected /= std::abs(Projected.x) + std::abs(Projected.y)
			+ std::abs(Projected.z);
		if (Projected.z < 0.0f)
		{
			const FVector2f Folded{
				(1.0f - std::abs(Projected.y))
					* (Projected.x >= 0.0f ? 1.0f : -1.0f),
				(1.0f - std::abs(Projected.x))
					* (Projected.y >= 0.0f ? 1.0f : -1.0f)};
			Projected.x = Folded.x;
			Projected.y = Folded.y;
		}
		return FVector2f(Projected) * 0.5f + 0.5f;
	}

	auto DecodeOctahedralNormal(const FVector2f& Encoded) -> FVector3f
	{
		const FVector2f Folded = Encoded * 2.0f - 1.0f;
		FVector3f Normal{
			Folded.x,
			Folded.y,
			1.0f - std::abs(Folded.x) - std::abs(Folded.y)};
		const float Correction = std::clamp(-Normal.z, 0.0f, 1.0f);
		Normal.x += Normal.x >= 0.0f ? -Correction : Correction;
		Normal.y += Normal.y >= 0.0f ? -Correction : Correction;
		return SafeNormal(Normal);
	}

	auto DecodeRecord(
		const FVector4f& Material,
		const FVector4f& Normals,
		const FVector4f& Surface,
		const FVector3f& Emissive) -> FDecodedRecord
	{
		return {
			.BaseColor = FVector3f(Material),
			.ShadingNormal = DecodeOctahedralNormal(
				{Normals.x, Normals.y}),
			.GeometricNormal = DecodeOctahedralNormal(
				{Normals.z, Normals.w}),
			.Emissive = Emissive,
			.Metallic = Material.w,
			.Roughness = Surface.x,
			.AmbientOcclusion = Surface.y,
			.EffectiveOpacity = Surface.z,
			.Flags = static_cast<uint8>(std::clamp(
				std::lround(Surface.w * 255.0f), 0l, 255l))};
	}

	auto DecodeR11G11B10Float(uint32 Packed) -> FVector3f
	{
		auto Decode = [](uint32 Bits, uint32 MantissaBits) {
			const uint32 MantissaMask = (1u << MantissaBits) - 1u;
			const uint32 Mantissa = Bits & MantissaMask;
			const uint32 Exponent = (Bits >> MantissaBits) & 0x1fu;
			if (Exponent == 0u)
			{
				return std::ldexp(static_cast<float>(Mantissa),
					1 - 15 - static_cast<int>(MantissaBits));
			}
			if (Exponent == 0x1fu)
			{
				return Mantissa == 0u
					? std::numeric_limits<float>::infinity()
					: std::numeric_limits<float>::quiet_NaN();
			}
			return std::ldexp(
				1.0f + static_cast<float>(Mantissa)
					/ static_cast<float>(1u << MantissaBits),
				static_cast<int>(Exponent) - 15);
		};
		return {
			Decode(Packed & 0x7ffu, 6u),
			Decode((Packed >> 11u) & 0x7ffu, 6u),
			Decode((Packed >> 22u) & 0x3ffu, 5u)};
	}

	auto ReconstructViewPositionAnalytic(
		const FMatrix& Projection,
		const FVector2f& Ndc,
		double DeviceDepth,
		FVector3& OutViewPosition) -> bool
	{
		const std::array<double, 3> Device{Ndc.x, Ndc.y, DeviceDepth};
		std::array<FVector3, 3> Rows;
		FVector3 Constants;
		for (size_t Row = 0; Row < Rows.size(); ++Row)
		{
			Rows[Row] = {
				Projection[0][Row] - Device[Row] * Projection[0][3],
				Projection[1][Row] - Device[Row] * Projection[1][3],
				Projection[2][Row] - Device[Row] * Projection[2][3]};
			Constants[Row] = -(
				Projection[3][Row] - Device[Row] * Projection[3][3]);
		}
		const double Denominator = Determinant(Rows);
		if (!std::isfinite(Denominator)
			|| std::abs(Denominator) <= std::numeric_limits<double>::min())
			return false;

		const std::array<FVector3, 3> XRows{
			FVector3{Constants.x, Rows[0].y, Rows[0].z},
			FVector3{Constants.y, Rows[1].y, Rows[1].z},
			FVector3{Constants.z, Rows[2].y, Rows[2].z}};
		const std::array<FVector3, 3> YRows{
			FVector3{Rows[0].x, Constants.x, Rows[0].z},
			FVector3{Rows[1].x, Constants.y, Rows[1].z},
			FVector3{Rows[2].x, Constants.z, Rows[2].z}};
		const std::array<FVector3, 3> ZRows{
			FVector3{Rows[0].x, Rows[0].y, Constants.x},
			FVector3{Rows[1].x, Rows[1].y, Constants.y},
			FVector3{Rows[2].x, Rows[2].y, Constants.z}};
		OutViewPosition = {
			Determinant(XRows) / Denominator,
			Determinant(YRows) / Denominator,
			Determinant(ZRows) / Denominator};
		return std::isfinite(OutViewPosition.x)
			&& std::isfinite(OutViewPosition.y)
			&& std::isfinite(OutViewPosition.z);
	}

	auto GetPositionTolerance(double DistanceToView) -> double
	{
		return std::max(0.002, 3.0e-5 * std::max(DistanceToView, 0.0));
	}
} // namespace Durin::GBufferContract
