#include <gtest/gtest.h>

#include "PrimitiveDrawInterface.h"
#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"
#include "Renderers/SimpleElement/EditorIconAtlas.h"

namespace Durin
{
	using RendererEditorAssistance::EDepthMode;
	using RendererEditorAssistance::EDrawOperation;
	using RendererEditorAssistance::EFeature;
	using RendererEditorAssistance::EGizmoTopology;
	using RendererEditorAssistance::FPipelineKey;
	using RendererEditorAssistance::FRequest;
	using RenderTargetLayouts::EViewportOutput;

	TEST(FRendererEditorAssistanceTests, EmptyViewRequestsNoAssistance)
	{
		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			FSceneView{}, EViewportOutput::Offscreen);
		EXPECT_TRUE(Request.IsEmpty());
		EXPECT_TRUE(FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request).empty());
		EXPECT_TRUE(FEditorAssistanceRenderer::BuildDrawableOperations(
			Request, {}).empty());
	}

	TEST(FEditorIconAtlasLayoutTests, UploadCoversEveryPackedIconTile)
	{
		EXPECT_EQ(FEditorIconAtlasLayout::Width, 192u);
		EXPECT_EQ(FEditorIconAtlasLayout::Height, 64u);
		EXPECT_EQ(FEditorIconAtlasLayout::RowPitchBytes, 192u * 4u);
		EXPECT_EQ(FEditorIconAtlasLayout::PixelByteCount,
			static_cast<size_t>(FEditorIconAtlasLayout::RowPitchBytes)
				* FEditorIconAtlasLayout::Height);
		EXPECT_EQ(FEditorIconAtlasLayout::GetTileX(0), 0u);
		EXPECT_EQ(FEditorIconAtlasLayout::GetTileX(1), 64u);
		EXPECT_EQ(FEditorIconAtlasLayout::GetTileX(2)
			+ FEditorIconAtlasLayout::IconExtent,
			FEditorIconAtlasLayout::Width);
	}

	TEST(FRendererEditorAssistanceTests,
		GridAndSolidGizmoRequestOnlySpecializedPipelines)
	{
		FSceneView View;
		View.EditorGrid.bVisible = true;
		View.DepthConvention = ESceneDepthConvention::ReversedZ;
		View.OverlayPrimitives.push_back({EViewOverlayShape::Arrow});
		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			View, EViewportOutput::Present);
		const std::vector<FPipelineKey> Keys =
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);
		ASSERT_EQ(Keys.size(), 3u);
		EXPECT_EQ(std::ranges::count_if(Keys, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::EditorGrid
				&& Key.DepthMode == EDepthMode::Visible;
		}), 1);
		EXPECT_EQ(std::ranges::count_if(Keys, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::Gizmo
				&& Key.GizmoTopology == EGizmoTopology::Solid;
		}), 2);
		EXPECT_TRUE(std::ranges::all_of(Keys, [](const FPipelineKey& Key) {
			return Key.Output == EViewportOutput::Present
				&& Key.DepthConvention == ESceneDepthConvention::ReversedZ;
		}));
	}

	TEST(FRendererEditorAssistanceTests,
		SimpleElementsRequestExplicitForegroundAndWorldOperations)
	{
		FSceneView View;
		FViewPrimitiveDrawInterface PDI(View);
		PDI.DrawPoint({0.0, 0.0, 1.0}, FVector4f(1.0f), 4.0f,
			ESceneDepthPriorityGroup::Foreground);
		PDI.DrawPoint({0.0, 0.0, 1.0}, FVector4f(1.0f), 4.0f,
			ESceneDepthPriorityGroup::World);
		PDI.Seal();
		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			View, EViewportOutput::Offscreen);
		EXPECT_TRUE(Request.bSimpleElements);
		EXPECT_TRUE(FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request).empty());
		const std::array Expected{
			EDrawOperation::ForegroundSimpleElements,
			EDrawOperation::WorldSimpleElements,
		};
		EXPECT_TRUE(std::ranges::equal(
			FEditorAssistanceRenderer::BuildDrawableOperations(Request, {}),
			Expected));
	}

	TEST(FRendererEditorAssistanceTests,
		DrawOrderKeepsGridFirstAndWorldSimpleElementsLast)
	{
		const std::span<const EDrawOperation> Order =
			FEditorAssistanceRenderer::GetDrawOrder();
		const std::array Expected{
			EDrawOperation::EditorGrid,
			EDrawOperation::XRayGizmos,
			EDrawOperation::ForegroundSimpleElements,
			EDrawOperation::VisibleGizmos,
			EDrawOperation::WorldSimpleElements,
		};
		EXPECT_TRUE(std::ranges::equal(Order, Expected));
		for (const EDrawOperation Operation : Order)
			EXPECT_EQ(std::ranges::count(Order, Operation), 1);
	}
} // namespace Durin
