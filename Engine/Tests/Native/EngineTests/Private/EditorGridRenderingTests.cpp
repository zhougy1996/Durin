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

	TEST(FEditorGridRenderingTests, BuildsMutuallyInverseShaderTransformsAndViewData)
	{
		FSceneView View;
		View.ViewProjectionMatrix = FMatrix(1.0);
		View.ViewProjectionMatrix[0][0] = 2.0;
		View.ViewProjectionMatrix[1][1] = 3.0;
		View.ViewProjectionMatrix[2][2] = 4.0;
		View.ViewProjectionMatrix[3] = FVector4(5.0, 6.0, 7.0, 1.0);
		View.ViewLocation = {11.0, 12.0, 13.0};
		View.EditorGrid.Height = 2.5f;
		View.EditorGrid.FadeDistance = 750.0f;
		View.EditorGrid.MinorColor = {0.1f, 0.2f, 0.3f, 0.4f};

		EditorGridRendering::FEditorGridUniform Uniform;
		ASSERT_TRUE(EditorGridRendering::BuildUniform(View, Uniform));
		ExpectIdentity(glm::transpose(Uniform.WorldToClip) * glm::transpose(Uniform.ClipToWorld));
		EXPECT_FLOAT_EQ(Uniform.GridPlane.x, 2.5f);
		EXPECT_FLOAT_EQ(Uniform.GridPlane.y, EditorGridRendering::GridDepthBias);
		EXPECT_GT(Uniform.GridPlane.y, 0.0f);
		EXPECT_EQ(Uniform.ViewPositionFadeDistance, FVector4f(11.0f, 12.0f, 13.0f, 750.0f));
		EXPECT_EQ(Uniform.MinorColor, View.EditorGrid.MinorColor);
	}

	TEST(FEditorGridRenderingTests, RejectsInvalidTransformsBeforeUniformUploadAndDraw)
	{
		FSceneView View;
		EditorGridRendering::FEditorGridUniform Uniform;

		View.ViewProjectionMatrix = FMatrix(0.0);
		EXPECT_FALSE(EditorGridRendering::BuildUniform(View, Uniform));

		View.ViewProjectionMatrix = FMatrix(1.0);
		View.ViewProjectionMatrix[0][0] = std::numeric_limits<double>::quiet_NaN();
		EXPECT_FALSE(EditorGridRendering::BuildUniform(View, Uniform));
	}
}
