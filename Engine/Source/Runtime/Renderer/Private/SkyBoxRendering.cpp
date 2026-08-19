#include "SkyBoxRendering.h"

#include "Math/Operations.h"

namespace Durin::SkyBoxRendering
{
	namespace
	{
		constexpr double MatrixInverseEpsilon = 1.e-8;

		auto IsFinite(const FMatrix4f& Matrix) -> bool
		{
			for (uint32 ColumnIndex = 0; ColumnIndex < 4; ++ColumnIndex)
			{
				const auto& Column = Matrix[ColumnIndex];
				if (!std::isfinite(Column.x) || !std::isfinite(Column.y)
					|| !std::isfinite(Column.z) || !std::isfinite(Column.w)) return false;
			}
			return true;
		}

	}

	auto BuildUniform(const FSceneView& View, const FSkyBoxSceneData& SkyBox, FSkyBoxUniform& OutUniform) -> bool
	{
		FMatrix ClipToWorld;
		FQuat NormalizedRotation;
		if (!Math::TryInverse(View.ViewProjectionMatrix, ClipToWorld, MatrixInverseEpsilon)
			|| !Math::TryNormalize(SkyBox.Rotation, NormalizedRotation, MatrixInverseEpsilon)) return false;

		const FMatrix WorldToSky = Math::RotationMatrix(Math::Inverse(NormalizedRotation));
		const FMatrix RemoveViewTranslation = Math::TranslationMatrix(-View.ViewLocation);
		const FMatrix ClipToSkyDirection = WorldToSky * RemoveViewTranslation * ClipToWorld;
		OutUniform.ClipToSkyDirection =
			Math::TransposeToFloat(ClipToSkyDirection);
		if (!IsFinite(OutUniform.ClipToSkyDirection)) return false;

		OutUniform.TintIntensity = FVector4f(SkyBox.Tint, std::max(0.0f, SkyBox.Intensity));
		return std::isfinite(OutUniform.TintIntensity.x)
			&& std::isfinite(OutUniform.TintIntensity.y) && std::isfinite(OutUniform.TintIntensity.z)
			&& std::isfinite(OutUniform.TintIntensity.w);
	}
}
