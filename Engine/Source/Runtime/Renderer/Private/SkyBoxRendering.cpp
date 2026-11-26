#include "SkyBoxRendering.h"

#include "Math/Operations.h"

namespace Durin::SkyBoxRendering
{
	namespace
	{
		constexpr double MatrixInverseEpsilon = 1.e-8;
	}

	auto BuildUniform(const FSceneView& View, const FViewSkyParameters& SkyBox, FSkyBoxUniform& OutUniform) -> bool
	{
		FMatrix ClipToWorld;
		FQuat NormalizedRotation;
		if (!Math::TryInverse(View.ViewProjectionMatrix, ClipToWorld, MatrixInverseEpsilon)
			|| !Math::TryNormalize(SkyBox.Rotation, NormalizedRotation, MatrixInverseEpsilon)) return false;

		const FMatrix WorldToSky = Math::RotationMatrix(Math::Inverse(NormalizedRotation));
		const FMatrix RemoveViewTranslation = Math::TranslationMatrix(-View.ViewLocation);
		const FMatrix ClipToSkyDirection = WorldToSky * RemoveViewTranslation * ClipToWorld;
		const float FarDepth = View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f;
		// At the sampled clip depth a ray is B * (ndc.x, ndc.y, 1).
		// Its inverse projects a dome vertex, including off-axis and orthographic views.
		glm::dmat3 Basis{FVector3(ClipToSkyDirection[0]), FVector3(ClipToSkyDirection[1]),
			FVector3(ClipToSkyDirection[2] + ClipToSkyDirection[3])};
		const FVector3 Normal = glm::normalize(glm::cross(Basis[0], Basis[1]));
		const double MinimumDepth = 1.e-6 * std::max(glm::length(Basis[0]), glm::length(Basis[1]));
		const double AxialDepth = glm::dot(Basis[2], Normal);
		if (std::abs(AxialDepth) < MinimumDepth)
			Basis[2] += Normal * (std::copysign(MinimumDepth, AxialDepth) - AxialDepth);
		const auto Rows = glm::transpose(glm::inverse(Basis));
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			const FVector3f Row(Rows[Axis]);
			if (!std::isfinite(Row.x) || !std::isfinite(Row.y) || !std::isfinite(Row.z)) return false;
			OutUniform.DomeProjection[Axis] = FVector4f(Row, Axis == 2 ? FarDepth : 0.0f);
		}
		OutUniform.TintIntensity = FVector4f(SkyBox.Tint, std::max(0.0f, SkyBox.Intensity));
		return std::isfinite(OutUniform.TintIntensity.x)
			&& std::isfinite(OutUniform.TintIntensity.y) && std::isfinite(OutUniform.TintIntensity.z)
			&& std::isfinite(OutUniform.TintIntensity.w);
	}
}
