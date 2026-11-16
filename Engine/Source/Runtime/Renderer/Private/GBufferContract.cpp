#include "GBufferContract.h"

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
		const FVector4f& ModelData
	) -> FDecodedRecord
	{
		const auto Flags = static_cast<uint8>(std::clamp(std::lround(Surface.w * 255.f), 0l, 255l));
		return {
			.BaseColor = FVector3f(Material),
			.ShadingNormal = DecodeOctahedralNormal(
				{Normals.x, Normals.y}
			),
			.GeometricNormal = DecodeOctahedralNormal(
				{Normals.z, Normals.w}
			),
			.Metallic = Surface.x,
			.Specular = (Flags & AuthoredSpecularFlag) ? std::clamp(Surface.y, 0.f, 1.f) : .5f,
			.ClearCoat = (Flags & 3u) == ClearCoatFlag ? std::clamp(ModelData.x, 0.f, 1.f) : 0.f,
			.ClearCoatRoughness = std::clamp(ModelData.y, .045f, 1.f),
			.Roughness = Surface.z,
			.AmbientOcclusion = Material.w,
			.EffectiveOpacity = 1.f,
			.Flags = Flags
		};
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
