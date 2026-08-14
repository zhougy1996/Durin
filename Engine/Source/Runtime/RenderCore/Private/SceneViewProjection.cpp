#include "SceneViewProjection.h"

#include "Math/Operations.h"

namespace Durin::SceneViewProjection
{
	namespace
	{
		constexpr double ProjectionEpsilon = 1.e-8;
	}

	auto IsValidPerspectiveClipRange(double NearClip, double FarClip) -> bool
	{
		return std::isfinite(NearClip) && std::isfinite(FarClip)
			&& NearClip >= 0.001 && FarClip > NearClip
			&& FarClip <= MaximumPerspectiveFarClip;
	}

	auto BuildPerspectiveProjection(double FieldOfViewDegrees,
		double AspectRatio, double NearClip, double FarClip,
		ESceneDepthConvention DepthConvention, FMatrix& OutProjection) -> bool
	{
		if (!std::isfinite(FieldOfViewDegrees) || FieldOfViewDegrees < 1.0
			|| FieldOfViewDegrees > 170.0 || !std::isfinite(AspectRatio)
			|| AspectRatio < 0.001 || !IsValidPerspectiveClipRange(NearClip, FarClip))
			return false;
		const double YScale = 1.0 / std::tan(
			Math::DegreesToRadians(FieldOfViewDegrees) * 0.5);
		const double Range = FarClip - NearClip;
		FMatrix Projection(0.0);
		Projection[1][0] = YScale / AspectRatio;
		Projection[2][1] = -YScale;
		Projection[0][2] = DepthConvention == ESceneDepthConvention::ReversedZ
			? -NearClip / Range : FarClip / Range;
		Projection[3][2] = DepthConvention == ESceneDepthConvention::ReversedZ
			? NearClip * FarClip / Range : -NearClip * FarClip / Range;
		Projection[0][3] = 1.0;
		OutProjection = Projection;
		return true;
	}

	auto ProjectWorldToViewport(const FSceneView& View, const FVector3& WorldPosition, FVector2f& OutPosition) -> bool
	{
		if (View.ViewportWidth == 0 || View.ViewportHeight == 0) return false;
		const FVector4 Clip = View.ViewProjectionMatrix * FVector4(WorldPosition, 1.0);
		if (!std::isfinite(Clip.w) || Clip.w <= ProjectionEpsilon) return false;
		const FVector2 Ndc = FVector2(Clip) / Clip.w;
		if (!std::isfinite(Ndc.x) || !std::isfinite(Ndc.y)) return false;
		OutPosition = {
			static_cast<float>(View.ViewportX + (Ndc.x + 1.0) * 0.5 * View.ViewportWidth),
			static_cast<float>(View.ViewportY + (Ndc.y + 1.0) * 0.5 * View.ViewportHeight)
		};
		return true;
	}

	auto BuildViewportRay(const FSceneView& View, const FVector2f& ViewportPosition, FVector3& OutOrigin, FVector3& OutDirection) -> bool
	{
		if (View.ViewportWidth == 0 || View.ViewportHeight == 0) return false;
		if (ViewportPosition.x < View.ViewportX || ViewportPosition.y < View.ViewportY
			|| ViewportPosition.x >= View.ViewportX + View.ViewportWidth || ViewportPosition.y >= View.ViewportY + View.ViewportHeight) return false;
		FMatrix ClipToWorld;
		if (!Math::TryInverse(View.ViewProjectionMatrix, ClipToWorld, ProjectionEpsilon)) return false;
		const double NdcX = (static_cast<double>(ViewportPosition.x) - View.ViewportX) / View.ViewportWidth * 2.0 - 1.0;
		const double NdcY = (static_cast<double>(ViewportPosition.y) - View.ViewportY) / View.ViewportHeight * 2.0 - 1.0;
		const double NearDepth = GetNearDeviceDepth(View.DepthConvention);
		const double FarDepth = GetFarDeviceDepth(View.DepthConvention);
		FVector4 Near = ClipToWorld * FVector4(NdcX, NdcY, NearDepth, 1.0);
		FVector4 Far = ClipToWorld * FVector4(NdcX, NdcY, FarDepth, 1.0);
		if (std::abs(Near.w) <= ProjectionEpsilon || std::abs(Far.w) <= ProjectionEpsilon) return false;
		Near /= Near.w;
		Far /= Far.w;
		OutOrigin = FVector3(Near);
		const FVector3 Delta = FVector3(Far) - OutOrigin;
		const double Length = Math::Length(Delta);
		if (!Math::IsFinite(OutOrigin) || !Math::IsFinite(Delta) || !std::isfinite(Length) || Length <= ProjectionEpsilon) return false;
		OutDirection = Delta / Length;
		return true;
	}
}
