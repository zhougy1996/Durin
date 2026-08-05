#include "SceneViewProjection.h"

#include "Math/Operations.h"

namespace Durin::SceneViewProjection
{
	namespace
	{
		constexpr double ProjectionEpsilon = 1.e-8;
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
		FVector4 Near = ClipToWorld * FVector4(NdcX, NdcY, 0.0, 1.0);
		FVector4 Far = ClipToWorld * FVector4(NdcX, NdcY, 1.0, 1.0);
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
