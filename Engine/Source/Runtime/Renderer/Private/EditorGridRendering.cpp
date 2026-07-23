#include "EditorGridRendering.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace Durin::EditorGridRendering
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

	auto BuildUniform(const FSceneView& View, FEditorGridUniform& OutUniform) -> bool
	{
		const double Determinant = glm::determinant(View.ViewProjectionMatrix);
		if (!std::isfinite(Determinant) || std::abs(Determinant) <= MatrixInverseEpsilon) return false;

		const glm::mat4 WorldToClip = ToShaderMatrix(View.ViewProjectionMatrix);
		const glm::mat4 ClipToWorld = ToShaderMatrix(glm::inverse(View.ViewProjectionMatrix));
		if (!IsFinite(WorldToClip) || !IsFinite(ClipToWorld)) return false;

		const float FadeDistance = std::max(1.0f, View.EditorGrid.FadeDistance);
		OutUniform.WorldToClip = WorldToClip;
		OutUniform.ClipToWorld = ClipToWorld;
		OutUniform.GridPlane = {static_cast<float>(View.EditorGrid.Height), 0.0f, 0.0f, 0.0f};
		OutUniform.ViewPositionFadeDistance = FVector4f(FVector3f(View.ViewLocation), FadeDistance);
		OutUniform.MinorColor = View.EditorGrid.MinorColor;
		OutUniform.MajorColor = View.EditorGrid.MajorColor;
		OutUniform.AxisXColor = View.EditorGrid.AxisXColor;
		OutUniform.AxisYColor = View.EditorGrid.AxisYColor;
		return true;
	}
}
