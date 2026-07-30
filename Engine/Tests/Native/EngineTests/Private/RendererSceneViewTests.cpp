#include <gtest/gtest.h>

#include "Renderers/SceneRenderer.h"

namespace Durin
{
	TEST(FRendererSceneViewTests, UnconstrainedViewsFitIndependentOutputs)
	{
		FSceneView View;
		View.ViewportX = 11;
		View.ViewportY = 13;
		View.ViewportWidth = 17;
		View.ViewportHeight = 19;

		const FSceneView MainView =
			FSceneRenderer::FitViewToOutput(View, 800, 600);
		const FSceneView AuxiliaryView =
			FSceneRenderer::FitViewToOutput(View, 320, 180);

		EXPECT_EQ(MainView.ViewportX, 0u);
		EXPECT_EQ(MainView.ViewportY, 0u);
		EXPECT_EQ(MainView.ViewportWidth, 800u);
		EXPECT_EQ(MainView.ViewportHeight, 600u);
		EXPECT_EQ(AuxiliaryView.ViewportX, 0u);
		EXPECT_EQ(AuxiliaryView.ViewportY, 0u);
		EXPECT_EQ(AuxiliaryView.ViewportWidth, 320u);
		EXPECT_EQ(AuxiliaryView.ViewportHeight, 180u);
		EXPECT_EQ(View.ViewportX, 11u);
		EXPECT_EQ(View.ViewportY, 13u);
		EXPECT_EQ(View.ViewportWidth, 17u);
		EXPECT_EQ(View.ViewportHeight, 19u);
	}

	TEST(FRendererSceneViewTests, FixedAspectViewsAreCenteredPerOutput)
	{
		FSceneView WideView;
		WideView.AspectRatioConstraint = 16.0f / 9.0f;
		const FSceneView WideResult =
			FSceneRenderer::FitViewToOutput(WideView, 800, 600);

		EXPECT_EQ(WideResult.ViewportX, 0u);
		EXPECT_EQ(WideResult.ViewportY, 75u);
		EXPECT_EQ(WideResult.ViewportWidth, 800u);
		EXPECT_EQ(WideResult.ViewportHeight, 450u);

		FSceneView TallView;
		TallView.AspectRatioConstraint = 0.5f;
		const FSceneView TallResult =
			FSceneRenderer::FitViewToOutput(TallView, 800, 600);

		EXPECT_EQ(TallResult.ViewportX, 250u);
		EXPECT_EQ(TallResult.ViewportY, 0u);
		EXPECT_EQ(TallResult.ViewportWidth, 300u);
		EXPECT_EQ(TallResult.ViewportHeight, 600u);
	}
} // namespace Durin
