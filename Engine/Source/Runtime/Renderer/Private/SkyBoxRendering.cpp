#include "SkyBoxRendering.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Durin::SkyBoxRendering
{
	namespace
	{
		constexpr double MatrixInverseEpsilon = 1.e-8;

		auto IsFinite(const glm::mat4& Matrix) -> bool
		{
			for (glm::length_t ColumnIndex = 0; ColumnIndex < 4; ++ColumnIndex)
			{
				const glm::vec4& Column = Matrix[ColumnIndex];
				if (!std::isfinite(Column.x) || !std::isfinite(Column.y)
					|| !std::isfinite(Column.z) || !std::isfinite(Column.w)) return false;
			}
			return true;
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> glm::mat4
		{
			return glm::transpose(glm::mat4(Matrix));
		}
	}

	auto BuildUniform(const FSceneView& View, const FSkyBoxSceneData& SkyBox, FSkyBoxUniform& OutUniform) -> bool
	{
		const double ViewProjectionDeterminant = glm::determinant(View.ViewProjectionMatrix);
		const double RotationLengthSquared = glm::dot(SkyBox.Rotation, SkyBox.Rotation);
		if (!std::isfinite(ViewProjectionDeterminant) || std::abs(ViewProjectionDeterminant) <= MatrixInverseEpsilon
			|| !std::isfinite(RotationLengthSquared) || RotationLengthSquared <= MatrixInverseEpsilon) return false;

		const FQuat NormalizedRotation = glm::normalize(SkyBox.Rotation);
		const FMatrix WorldToSky = glm::mat4_cast(glm::inverse(NormalizedRotation));
		OutUniform.ClipToWorld = ToShaderMatrix(glm::inverse(View.ViewProjectionMatrix));
		OutUniform.WorldToSky = ToShaderMatrix(WorldToSky);
		if (!IsFinite(OutUniform.ClipToWorld) || !IsFinite(OutUniform.WorldToSky)) return false;

		OutUniform.ViewPosition = FVector4f(FVector3f(View.ViewLocation), 0.0f);
		OutUniform.TintIntensity = FVector4f(SkyBox.Tint, std::max(0.0f, SkyBox.Intensity));
		return std::isfinite(OutUniform.ViewPosition.x) && std::isfinite(OutUniform.ViewPosition.y)
			&& std::isfinite(OutUniform.ViewPosition.z) && std::isfinite(OutUniform.TintIntensity.x)
			&& std::isfinite(OutUniform.TintIntensity.y) && std::isfinite(OutUniform.TintIntensity.z)
			&& std::isfinite(OutUniform.TintIntensity.w);
	}
}
