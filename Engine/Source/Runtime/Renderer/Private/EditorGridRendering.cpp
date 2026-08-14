#include "EditorGridRendering.h"

#include "Math/Operations.h"

namespace Durin::EditorGridRendering
{
	namespace
	{
		constexpr double MatrixInverseEpsilon = 1.e-8;

		auto PositiveModulo(double Value, double Period) -> double
		{
			double Result = std::fmod(Value, Period);
			return Result < 0.0 ? Result + Period : Result;
		}

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
		FMatrix RelativeViewMatrix = View.ViewMatrix;
		RelativeViewMatrix[3][0] = 0.0;
		RelativeViewMatrix[3][1] = 0.0;
		RelativeViewMatrix[3][2] = 0.0;
		const FMatrix RelativeWorldToClipMatrix =
			View.ProjectionMatrix * RelativeViewMatrix;
		FMatrix ClipToRelativeWorldMatrix;
		if (!Math::TryInverse(
				RelativeWorldToClipMatrix,
				ClipToRelativeWorldMatrix,
				MatrixInverseEpsilon))
		{
			return false;
		}

		const FMatrix4f RelativeWorldToClip =
			ToShaderMatrix(RelativeWorldToClipMatrix);
		const FMatrix4f ClipToRelativeWorld =
			ToShaderMatrix(ClipToRelativeWorldMatrix);
		if (!IsFinite(RelativeWorldToClip)
			|| !IsFinite(ClipToRelativeWorld))
		{
			return false;
		}

		const double RelativeGridHeight =
			View.EditorGrid.Height - View.ViewLocation.z;
		if (!std::isfinite(RelativeGridHeight)) return false;
		const FVector4 ClipPlane = glm::transpose(ClipToRelativeWorldMatrix)
			* FVector4(0.0, 0.0, 1.0, -RelativeGridHeight);
		if (!std::isfinite(ClipPlane.x) || !std::isfinite(ClipPlane.y)
			|| !std::isfinite(ClipPlane.z) || !std::isfinite(ClipPlane.w))
		{
			return false;
		}

		const float FadeDistance = std::max(1.0f, View.EditorGrid.FadeDistance);
		OutUniform.RelativeWorldToClip = RelativeWorldToClip;
		OutUniform.ClipToRelativeWorld = ClipToRelativeWorld;
		OutUniform.GridPlane = {
			static_cast<float>(RelativeGridHeight), 0.0f,
			View.DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0f : 0.0f,
			0.0f};
		OutUniform.ViewPositionFadeDistance = FVector4f(FVector3f(View.ViewLocation), FadeDistance);
		for (int32 Exponent = MinimumGridExponent;
			Exponent <= MaximumGridExponent;
			++Exponent)
		{
			const double Spacing = std::pow(10.0, Exponent);
			const uint32 Index = static_cast<uint32>(Exponent - MinimumGridExponent);
			OutUniform.GridPhases[Index] = {
				static_cast<float>(PositiveModulo(View.ViewLocation.x, Spacing) / Spacing),
				static_cast<float>(PositiveModulo(View.ViewLocation.y, Spacing) / Spacing),
				0.0f,
				0.0f};
		}
		OutUniform.MinorColor = View.EditorGrid.MinorColor;
		OutUniform.MajorColor = View.EditorGrid.MajorColor;
		OutUniform.AxisXColor = View.EditorGrid.AxisXColor;
		OutUniform.AxisYColor = View.EditorGrid.AxisYColor;
		OutUniform.ClipPlane = FVector4f(ClipPlane);
		return true;
	}
}
