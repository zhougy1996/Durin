#include <gtest/gtest.h>

#include "PrimitiveDrawInterface.h"
#include "Renderers/SimpleElement/SimpleElementCollector.h"

namespace Durin
{
	namespace
	{
		auto MakeView() -> FSceneView
		{
			FSceneView View;
			View.ViewProjectionMatrix = FMatrix(1.0);
			View.ViewportWidth = 200;
			View.ViewportHeight = 100;
			return View;
		}
	} // namespace

	TEST(FSimpleElementCollectorTests,
		BuildsStableDepthBlendAndTextureBatches)
	{
		FSceneView View = MakeView();
		FViewPrimitiveDrawInterface PDI(View);
		PDI.DrawLine({-0.5, 0.0, 0.5}, {0.5, 0.0, 0.5},
			FVector4f(1.0f), ESceneDepthPriorityGroup::World,
			{.WidthPixels = 4.0f,
				.Pattern = ESimpleElementLinePattern::Dashed,
				.PatternPeriodPixels = 8.0f});
		PDI.DrawPoint({0.0, 0.0, 0.5}, FVector4f(1.0f), 6.0f,
			ESceneDepthPriorityGroup::Foreground);
		PDI.DrawSprite({0.0, 0.0, 0.5}, {20.0f, 10.0f},
			FSimpleElementTexture::EditorIconAtlas(), {0.0f, 0.0f},
			{0.25f, 1.0f}, FVector4f(1.0f),
			ESceneDepthPriorityGroup::Foreground);
		PDI.Seal();

		const FPreparedSimpleElements Prepared =
			FSimpleElementCollector::Collect(View,
				RenderTargetLayouts::EViewportOutput::Offscreen);
		ASSERT_EQ(Prepared.Statistics.AcceptedElementCount, 3u);
		ASSERT_EQ(Prepared.Statistics.DroppedElementCount, 0u);
		ASSERT_EQ(Prepared.Batches.size(), 3u);
		EXPECT_EQ(Prepared.Batches[0].Key.DepthPriorityGroup,
			ESceneDepthPriorityGroup::Foreground);
		EXPECT_EQ(Prepared.Batches[0].Key.ShaderClass,
			ESimpleElementShaderClass::Untextured);
		EXPECT_EQ(Prepared.Batches[1].Key.ShaderClass,
			ESimpleElementShaderClass::Textured);
		EXPECT_EQ(Prepared.Batches[2].Key.DepthPriorityGroup,
			ESceneDepthPriorityGroup::World);
		for (const FPreparedSimpleElementBatch& Batch : Prepared.Batches)
		{
			EXPECT_EQ(Batch.Vertices.size(), 4u);
			EXPECT_EQ(Batch.Indices.size(), 6u);
			EXPECT_EQ(Batch.VertexBytes,
				4u * sizeof(FSimpleElementVertex));
			EXPECT_EQ(Batch.IndexBytes, 6u * sizeof(uint32));
		}
		const auto& LineBatch = Prepared.Batches[2];
		EXPECT_FLOAT_EQ(LineBatch.Vertices[0].Pattern.y, 8.0f);
		EXPECT_NE(LineBatch.Vertices[0].Position.y,
			LineBatch.Vertices[1].Position.y);
	}

	TEST(FSimpleElementCollectorTests,
		PreservesExplicitForegroundAndWorldPairs)
	{
		FSceneView View = MakeView();
		FViewPrimitiveDrawInterface PDI(View);
		const FVector4f ForegroundColor{1.0f, 1.0f, 1.0f, 0.3f};
		PDI.DrawTranslucentLine({-0.5, 0.0, 0.5}, {0.5, 0.0, 0.5},
			ForegroundColor, ESceneDepthPriorityGroup::Foreground,
			{.WidthPixels = 2.0f});
		PDI.DrawLine({-0.5, 0.0, 0.5}, {0.5, 0.0, 0.5},
			FVector4f(1.0f), ESceneDepthPriorityGroup::World,
			{.WidthPixels = 2.0f});
		PDI.Seal();
		const FPreparedSimpleElements Prepared =
			FSimpleElementCollector::Collect(View,
				RenderTargetLayouts::EViewportOutput::Present);
		EXPECT_EQ(Prepared.Statistics.SubmittedElementCount, 2u);
		EXPECT_EQ(Prepared.Statistics.AcceptedElementCount, 2u);
		ASSERT_EQ(Prepared.Batches.size(), 2u);
		EXPECT_EQ(Prepared.Batches[0].Key.DepthPriorityGroup,
			ESceneDepthPriorityGroup::Foreground);
		EXPECT_FLOAT_EQ(Prepared.Batches[0].Vertices[0].Color.w, 0.3f);
		EXPECT_EQ(Prepared.Batches[1].Key.DepthPriorityGroup,
			ESceneDepthPriorityGroup::World);
		EXPECT_FLOAT_EQ(Prepared.Batches[1].Vertices[0].Color.w, 1.0f);
	}

	TEST(FSimpleElementCollectorTests,
		RejectsOneInvalidElementWithoutSuppressingIndependentGeometry)
	{
		FSceneView View = MakeView();
		FViewPrimitiveDrawInterface PDI(View);
		PDI.DrawPoint({0.0, 0.0, -1.0}, FVector4f(1.0f), 5.0f);
		PDI.DrawPoint({0.0, 0.0, 0.5}, FVector4f(1.0f), 5.0f);
		PDI.Seal();
		const FPreparedSimpleElements Prepared =
			FSimpleElementCollector::Collect(View,
				RenderTargetLayouts::EViewportOutput::Offscreen);
		EXPECT_EQ(Prepared.Statistics.AcceptedElementCount, 1u);
		EXPECT_EQ(Prepared.Statistics.DroppedElementCount, 1u);
		ASSERT_EQ(Prepared.Batches.size(), 1u);
		EXPECT_EQ(Prepared.Batches[0].SourceElementCount, 1u);
	}
} // namespace Durin
