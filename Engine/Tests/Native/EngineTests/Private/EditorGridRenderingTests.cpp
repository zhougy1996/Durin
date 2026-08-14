#include <gtest/gtest.h>

#include "EditorGridRendering.h"

namespace Durin
{
	namespace
	{
		auto ExpectIdentity(const FMatrix4f& Matrix, float Tolerance = 1.e-5f) -> void
		{
			for (glm::length_t Column = 0; Column < 4; ++Column)
			{
				for (glm::length_t Row = 0; Row < 4; ++Row)
				{
					EXPECT_NEAR(Matrix[Column][Row], Column == Row ? 1.0f : 0.0f, Tolerance);
				}
			}
		}
	}

	TEST(FEditorGridRenderingTests, BuildsCameraRelativeTransformsPhasesAndViewData)
	{
		FSceneView View;
		View.ViewMatrix = FMatrix(1.0);
		View.ViewMatrix[3] = FVector4(-1000000.25, 2000000.75, -13.0, 1.0);
		View.ProjectionMatrix = FMatrix(1.0);
		View.ProjectionMatrix[0][0] = 2.0;
		View.ProjectionMatrix[1][1] = 3.0;
		View.ProjectionMatrix[2][2] = 4.0;
		View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
		View.ViewLocation = {1000000.25, -2000000.75, 13.0};
		View.EditorGrid.Height = 2.5f;
		View.EditorGrid.FadeDistance = 750.0f;
		View.EditorGrid.MinorColor = {0.1f, 0.2f, 0.3f, 0.4f};

		EditorGridRendering::FEditorGridUniform Uniform;
		ASSERT_TRUE(EditorGridRendering::BuildUniform(View, Uniform));
		ExpectIdentity(glm::transpose(Uniform.RelativeWorldToClip)
			* glm::transpose(Uniform.ClipToRelativeWorld));
		EXPECT_FLOAT_EQ(Uniform.GridPlane.x, -10.5f);
		EXPECT_FLOAT_EQ(Uniform.GridPlane.y, 0.0f);
		EXPECT_FLOAT_EQ(Uniform.GridPlane.z, 0.0f);
		EXPECT_EQ(Uniform.ViewPositionFadeDistance,
			FVector4f(1000000.25f, -2000000.75f, 13.0f, 750.0f));
		const uint32 UnitPhaseIndex = static_cast<uint32>(
			-EditorGridRendering::MinimumGridExponent);
		EXPECT_FLOAT_EQ(Uniform.GridPhases[UnitPhaseIndex].x, 0.25f);
		EXPECT_FLOAT_EQ(Uniform.GridPhases[UnitPhaseIndex].y, 0.25f);
		EXPECT_FLOAT_EQ(Uniform.GridPhases[UnitPhaseIndex + 2].x, 0.0025f);
		EXPECT_FLOAT_EQ(Uniform.GridPhases[UnitPhaseIndex + 2].y, 0.9925f);
		EXPECT_EQ(Uniform.MinorColor, View.EditorGrid.MinorColor);
		EXPECT_TRUE(std::isfinite(Uniform.ClipPlane.x));
		EXPECT_TRUE(std::isfinite(Uniform.ClipPlane.y));
		EXPECT_TRUE(std::isfinite(Uniform.ClipPlane.z));
		EXPECT_TRUE(std::isfinite(Uniform.ClipPlane.w));
		EXPECT_FLOAT_EQ(Uniform.ClipPlane.x, 0.0f);
		EXPECT_FLOAT_EQ(Uniform.ClipPlane.y, 0.0f);
		EXPECT_FLOAT_EQ(Uniform.ClipPlane.z, 0.25f);
		EXPECT_FLOAT_EQ(Uniform.ClipPlane.w, 10.5f);
		View.DepthConvention = ESceneDepthConvention::ReversedZ;
		ASSERT_TRUE(EditorGridRendering::BuildUniform(View, Uniform));
		EXPECT_FLOAT_EQ(Uniform.GridPlane.z, 1.0f);
	}

	TEST(FEditorGridRenderingTests, RejectsInvalidTransformsBeforeUniformUploadAndDraw)
	{
		FSceneView View;
		EditorGridRendering::FEditorGridUniform Uniform;

		View.ProjectionMatrix = FMatrix(0.0);
		EXPECT_FALSE(EditorGridRendering::BuildUniform(View, Uniform));

		View.ProjectionMatrix = FMatrix(1.0);
		View.ViewMatrix[0][0] = std::numeric_limits<double>::quiet_NaN();
		EXPECT_FALSE(EditorGridRendering::BuildUniform(View, Uniform));
	}
}
