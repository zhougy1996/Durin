#include "EditorGridRendering.h"

#include "Math/Operations.h"

namespace Durin::EditorGridRendering
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

	auto BuildUniform(const FSceneView& View, FEditorGridUniform& OutUniform) -> bool
	{
		FMatrix ClipToWorldMatrix;
		if (!Math::TryInverse(View.ViewProjectionMatrix, ClipToWorldMatrix, MatrixInverseEpsilon)) return false;

		const FMatrix4f WorldToClip = ToShaderMatrix(View.ViewProjectionMatrix);
		const FMatrix4f ClipToWorld = ToShaderMatrix(ClipToWorldMatrix);
		if (!IsFinite(WorldToClip) || !IsFinite(ClipToWorld)) return false;

		const float FadeDistance = std::max(1.0f, View.EditorGrid.FadeDistance);
		OutUniform.WorldToClip = WorldToClip;
		OutUniform.ClipToWorld = ClipToWorld;
		OutUniform.GridPlane = {
			static_cast<float>(View.EditorGrid.Height), GridDepthBias, 0.0f, 0.0f};
		OutUniform.ViewPositionFadeDistance = FVector4f(FVector3f(View.ViewLocation), FadeDistance);
		OutUniform.MinorColor = View.EditorGrid.MinorColor;
		OutUniform.MajorColor = View.EditorGrid.MajorColor;
		OutUniform.AxisXColor = View.EditorGrid.AxisXColor;
		OutUniform.AxisYColor = View.EditorGrid.AxisYColor;
		return true;
	}
}
