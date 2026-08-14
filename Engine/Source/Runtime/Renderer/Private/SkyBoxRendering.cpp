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

		auto ToShaderMatrix(const FMatrix& Matrix) -> FMatrix4f
		{
			FMatrix4f Result(0.0f);
			for (uint32 Column = 0; Column < 4; ++Column)
			{
				for (uint32 Row = 0; Row < 4; ++Row)
				{
					Result[Column][Row] = static_cast<float>(Matrix[Row][Column]);
				}
			}
			return Result;
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
		OutUniform.ClipToSkyDirection = ToShaderMatrix(ClipToSkyDirection);
		if (!IsFinite(OutUniform.ClipToSkyDirection)) return false;

		OutUniform.TintIntensity = FVector4f(SkyBox.Tint, std::max(0.0f, SkyBox.Intensity));
		return std::isfinite(OutUniform.TintIntensity.x)
			&& std::isfinite(OutUniform.TintIntensity.y) && std::isfinite(OutUniform.TintIntensity.z)
			&& std::isfinite(OutUniform.TintIntensity.w);
	}
}
