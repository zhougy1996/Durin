#include <gtest/gtest.h>

#include "Resources/RenderTargetLayouts.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "Renderers/ContactShadowRenderer.h"

#include <limits>

namespace Durin
{
	using namespace RenderTargetLayouts;

	TEST(FRendererRenderTargetLayoutTests, SceneTargetsPreserveDepthForEditorAssistance)
	{
		const FRHIRenderTargetLayout Layout = MakeSceneTargets();

		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 1u);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.Format, EPixelFormat::RGBA16_FLOAT);
		ASSERT_TRUE(Layout.bHasDepthStencil);
		EXPECT_EQ(Layout.DepthStencilAttachment.Format, EPixelFormat::D32);
		EXPECT_EQ(Layout.DepthStencilAttachment.LoadAction, ERHIRenderTargetLoadAction::Clear);
		EXPECT_EQ(Layout.DepthStencilAttachment.StoreAction, ERHIRenderTargetStoreAction::Store);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalLayout, ERHITextureLayout::DepthStencilAttachment);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalAccess, ERHIAccess::DepthStencilReadWrite);
	}

	TEST(FRendererRenderTargetLayoutTests, ContactVisibilityFreezesSingleChannelContract)
	{
		const FRHIRenderTargetLayout Layout = MakeContactVisibilityOutput();
		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 1u);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.Format, EPixelFormat::R8_UNORM);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(FContactShadowVisibilityRenderer::BytesPerPixel, 1u);
		EXPECT_EQ(FContactShadowVisibilityRenderer::CalculateTargetBytes(1920, 1080), 2'073'600u);
		EXPECT_EQ(FContactShadowVisibilityRenderer::MaximumRetainedBytes,
			32u * 1024u * 1024u);
	}

	TEST(FRendererRenderTargetLayoutTests, ContactVisibilityRouteTableIsPureAndBounded)
	{
		using FRenderer = FContactShadowVisibilityRenderer;
		auto MakeEligible = [] {
			return FRenderer::FRouteInputs{
				.bRequested = true,
				.bInputsValid = true,
				.bComputePayloadReady = true,
				.bComputeTargetReady = true,
				.bFragmentReady = true,
				.Width = 1921,
				.Height = 1081,
				.MaxGroupCountX = 65'535,
				.MaxGroupCountY = 65'535};
		};
		auto Inputs = MakeEligible();
		auto Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::Compute);
		EXPECT_EQ(Decision.Reason, FRenderer::ERouteReason::Compute);
		EXPECT_EQ(FRenderer::CalculateGroupCount(1921), 241u);
		EXPECT_EQ(FRenderer::CalculateGroupCount(1081), 136u);

		Inputs = MakeEligible();
		Inputs.bRequested = false;
		Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::FactorOne);
		EXPECT_EQ(Decision.Reason, FRenderer::ERouteReason::DisabledOrUnneeded);

		Inputs = MakeEligible();
		Inputs.bInputsValid = false;
		Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::FactorOne);
		EXPECT_EQ(Decision.Reason, FRenderer::ERouteReason::InvalidInputs);

		Inputs = MakeEligible();
		Inputs.Width = 0;
		Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::FactorOne);
		EXPECT_EQ(Decision.Reason, FRenderer::ERouteReason::InvalidExtent);

		Inputs = MakeEligible();
		Inputs.bComputePayloadReady = false;
		Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::Fragment);
		EXPECT_EQ(Decision.Reason,
			FRenderer::ERouteReason::ComputePayloadUnavailable);

		Inputs = MakeEligible();
		Inputs.bComputeTargetReady = false;
		Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::Fragment);
		EXPECT_EQ(Decision.Reason,
			FRenderer::ERouteReason::ComputeTargetUnavailable);

		Inputs = MakeEligible();
		Inputs.MaxGroupCountX = 240;
		Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::Fragment);
		EXPECT_EQ(Decision.Reason,
			FRenderer::ERouteReason::ComputeExtentUnsupported);

		Inputs.bFragmentReady = false;
		Decision = FRenderer::SelectRoute(Inputs);
		EXPECT_EQ(Decision.Route, FRenderer::ERoute::FactorOne);
		EXPECT_EQ(Decision.Reason, FRenderer::ERouteReason::FragmentUnavailable);
	}

	TEST(FRendererRenderTargetLayoutTests, GBufferTargetsFreezeFormatsStatesAndByteBudget)
	{
		const FRHIRenderTargetLayout Layout = MakeGBufferTargets();
		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 4u);
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			EXPECT_EQ(Layout.ColorAttachments[Index].RenderTarget.Format, EPixelFormat::RGBA8_UNORM);
			EXPECT_EQ(Layout.ColorAttachments[Index].RenderTarget.LoadAction, ERHIRenderTargetLoadAction::Clear);
			EXPECT_EQ(Layout.ColorAttachments[Index].RenderTarget.StoreAction, ERHIRenderTargetStoreAction::Store);
			EXPECT_EQ(Layout.ColorAttachments[Index].RenderTarget.FinalLayout, ERHITextureLayout::ShaderReadOnly);
			EXPECT_EQ(Layout.ColorAttachments[Index].RenderTarget.FinalAccess, ERHIAccess::GraphicsShaderRead);
		}
		EXPECT_EQ(Layout.ColorAttachments[3].RenderTarget.Format, EPixelFormat::R11G11B10_FLOAT);
		ASSERT_TRUE(Layout.bHasDepthStencil);
		EXPECT_EQ(Layout.DepthStencilAttachment.Format, EPixelFormat::D32);
		EXPECT_EQ(Layout.DepthStencilAttachment.StoreAction, ERHIRenderTargetStoreAction::Store);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalAccess, ERHIAccess::GraphicsShaderRead);

		EXPECT_EQ(FGBufferRenderer::BytesPerPixel, 16u);
		EXPECT_EQ(FGBufferRenderer::CalculateTargetBytes(1920, 1080), 33'177'600u);
		EXPECT_GE(FGBufferRenderer::MaximumRetainedBytes, 4u * FGBufferRenderer::CalculateTargetBytes(1920, 1080));
		EXPECT_LT(FGBufferRenderer::MaximumRetainedBytes, 5u * FGBufferRenderer::CalculateTargetBytes(1920, 1080));
		EXPECT_EQ(FGBufferRenderer::CalculateTargetBytes(std::numeric_limits<uint32>::max(), std::numeric_limits<uint32>::max()), std::numeric_limits<uint64>::max());
	}

	TEST(FRendererRenderTargetLayoutTests, SceneTargetByteBudgetIsFormatAware)
	{
		EXPECT_EQ(FPostProcessRenderer::SceneTargetBytesPerPixel, 12u);
		EXPECT_EQ(
			FPostProcessRenderer::CalculateSceneTargetBytes(1920, 1080),
			24'883'200u
		);
		EXPECT_GE(
			FPostProcessRenderer::MaximumRetainedSceneTargetBytes,
			3u * FPostProcessRenderer::CalculateSceneTargetBytes(1920, 1080)
		);
		EXPECT_LT(
			FPostProcessRenderer::MaximumRetainedSceneTargetBytes,
			5u * FPostProcessRenderer::CalculateSceneTargetBytes(1920, 1080)
		);
		EXPECT_EQ(
			FPostProcessRenderer::CalculateSceneTargetBytes(
				std::numeric_limits<uint32>::max(),
				std::numeric_limits<uint32>::max()
			),
			std::numeric_limits<uint64>::max()
		);
	}

	TEST(FRendererRenderTargetLayoutTests, DeferredDirectionalTargetFreezesLayoutUniformAndByteBudget)
	{
		const FRHIRenderTargetLayout Layout =
			MakeDeferredDirectionalOutput();
		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 1u);
		EXPECT_FALSE(Layout.bHasDepthStencil);
		const FRHIAttachmentLayout& Color =
			Layout.ColorAttachments[0].RenderTarget;
		EXPECT_EQ(Color.Format, EPixelFormat::RGBA16_FLOAT);
		EXPECT_EQ(Color.LoadAction, ERHIRenderTargetLoadAction::Clear);
		EXPECT_EQ(Color.StoreAction, ERHIRenderTargetStoreAction::Store);
		EXPECT_EQ(Color.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Color.FinalAccess, ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(sizeof(FDeferredDirectionalLightingRenderer::FViewUniform), 176u);
		EXPECT_EQ(FDeferredDirectionalLightingRenderer::BytesPerPixel, 8u);
		EXPECT_EQ(FDeferredDirectionalLightingRenderer::CalculateTargetBytes(1920, 1080), 16'588'800u);
		EXPECT_GE(FDeferredDirectionalLightingRenderer::MaximumRetainedBytes, 4u * FDeferredDirectionalLightingRenderer::CalculateTargetBytes(1920, 1080));
		EXPECT_LT(FDeferredDirectionalLightingRenderer::MaximumRetainedBytes, 5u * FDeferredDirectionalLightingRenderer::CalculateTargetBytes(1920, 1080));
		EXPECT_EQ(FDeferredDirectionalLightingRenderer::CalculateTargetBytes(std::numeric_limits<uint32>::max(), std::numeric_limits<uint32>::max()), std::numeric_limits<uint64>::max());
	}

	TEST(FRendererRenderTargetLayoutTests, HybridProductionLayoutsPreserveGBufferDepthAndLoadSceneColor)
	{
		const FRHIRenderTargetLayout Bootstrap = MakeHybridSceneBootstrap();
		const FRHIRenderTargetLayout Deferred = MakeHybridDeferredOutput();
		const FRHIRenderTargetLayout Retained = MakeHybridRetainedForward();
		ASSERT_TRUE(Bootstrap.IsValid());
		ASSERT_TRUE(Deferred.IsValid());
		ASSERT_TRUE(Retained.IsValid());
		EXPECT_EQ(Bootstrap.NumColorRenderTargets, 1u);
		EXPECT_EQ(Bootstrap.ColorAttachments[0].RenderTarget.LoadAction, ERHIRenderTargetLoadAction::Clear);
		EXPECT_TRUE(Bootstrap.bHasDepthStencil);
		EXPECT_EQ(Bootstrap.DepthStencilAttachment.LoadAction, ERHIRenderTargetLoadAction::Load);
		EXPECT_EQ(Bootstrap.DepthStencilAttachment.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Deferred.NumColorRenderTargets, 1u);
		EXPECT_FALSE(Deferred.bHasDepthStencil);
		EXPECT_EQ(Deferred.ColorAttachments[0].RenderTarget.LoadAction, ERHIRenderTargetLoadAction::Load);
		EXPECT_TRUE(Retained.bHasDepthStencil);
		EXPECT_EQ(Retained.DepthStencilAttachment.LoadAction, ERHIRenderTargetLoadAction::Load);
		EXPECT_EQ(Retained.DepthStencilAttachment.FinalLayout, ERHITextureLayout::DepthStencilAttachment);
		EXPECT_EQ(Retained.DepthStencilAttachment.FinalAccess, ERHIAccess::DepthStencilReadWrite);
		EXPECT_EQ(Retained.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::ShaderReadOnly);
	}

	TEST(FRendererRenderTargetLayoutTests, ScenePostProcessLeavesColorReadyForEditorAssistance)
	{
		const FRHIRenderTargetLayout Layout = MakeScenePostProcessOutput();

		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 1);
		EXPECT_FALSE(Layout.bHasDepthStencil);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::ColorAttachment);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::ColorAttachmentReadWrite);
	}

	TEST(FRendererRenderTargetLayoutTests, DirectionalShadowDepthPublishesStoredD32ForFragmentSampling)
	{
		const FRHIRenderTargetLayout Layout = MakeDirectionalShadowDepth();
		EXPECT_EQ(Layout.NumColorRenderTargets, 0u);
		ASSERT_TRUE(Layout.bHasDepthStencil);
		const FRHIAttachmentLayout& Depth = Layout.DepthStencilAttachment;
		EXPECT_EQ(Depth.Format, EPixelFormat::D32);
		EXPECT_EQ(Depth.LoadAction, ERHIRenderTargetLoadAction::Clear);
		EXPECT_EQ(Depth.StoreAction, ERHIRenderTargetStoreAction::Store);
		EXPECT_EQ(Depth.InitialLayout, ERHITextureLayout::Undefined);
		EXPECT_EQ(Depth.InitialAccess, ERHIAccess::None);
		EXPECT_EQ(Depth.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Depth.FinalAccess, ERHIAccess::GraphicsShaderRead);
	}

	TEST(FRendererRenderTargetLayoutTests, FinalScenePostProcessOwnsOutputTransition)
	{
		const FRHIRenderTargetLayout Offscreen =
			MakeFinalScenePostProcessOutput(EViewportOutput::Offscreen);
		const FRHIRenderTargetLayout Present =
			MakeFinalScenePostProcessOutput(EViewportOutput::Present);

		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::Present);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::Present);
	}

	TEST(FRendererRenderTargetLayoutTests, EditorAssistanceOutputLoadsPreservedColorAndDepth)
	{
		for (const EViewportOutput Output : {EViewportOutput::Offscreen, EViewportOutput::Present})
		{
			const FRHIRenderTargetLayout Layout = MakeEditorAssistanceOutput(Output);

			ASSERT_TRUE(Layout.IsValid());
			ASSERT_EQ(Layout.NumColorRenderTargets, 1);
			ASSERT_TRUE(Layout.bHasDepthStencil);
			EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.LoadAction, ERHIRenderTargetLoadAction::Load);
			EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.InitialLayout, ERHITextureLayout::ColorAttachment);
			EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.InitialAccess, ERHIAccess::ColorAttachmentReadWrite);
			EXPECT_EQ(Layout.DepthStencilAttachment.Format, EPixelFormat::D32);
			EXPECT_EQ(Layout.DepthStencilAttachment.LoadAction, ERHIRenderTargetLoadAction::Load);
			EXPECT_EQ(Layout.DepthStencilAttachment.StoreAction, ERHIRenderTargetStoreAction::DontCare);
			EXPECT_EQ(Layout.DepthStencilAttachment.InitialLayout, ERHITextureLayout::DepthStencilAttachment);
			EXPECT_EQ(Layout.DepthStencilAttachment.InitialAccess, ERHIAccess::DepthStencilReadWrite);
		}
	}

	TEST(FRendererRenderTargetLayoutTests, GroundTruthAmbientOcclusionRawTargetFreezesLayoutAndBudget)
	{
		const FRHIRenderTargetLayout Layout =
			MakeGroundTruthAmbientOcclusionOutput();
		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 1u);
		EXPECT_FALSE(Layout.bHasDepthStencil);
		const FRHIAttachmentLayout& Color =
			Layout.ColorAttachments[0].RenderTarget;
		EXPECT_EQ(Color.Format, EPixelFormat::R8_UNORM);
		EXPECT_EQ(Color.LoadAction, ERHIRenderTargetLoadAction::Clear);
		EXPECT_EQ(Color.StoreAction, ERHIRenderTargetStoreAction::Store);
		EXPECT_EQ(Color.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Color.FinalAccess, ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(sizeof(FGroundTruthAmbientOcclusionRenderer::FViewUniform), 176u);
		EXPECT_EQ(sizeof(FGroundTruthAmbientOcclusionRenderer::FFilterUniform), 112u);
		EXPECT_EQ(FGroundTruthAmbientOcclusionRenderer::BytesPerPixel, 1u);
		EXPECT_EQ(
			FGroundTruthAmbientOcclusionRenderer::CalculateRawTargetBytes(1920, 1080),
			2'073'600u);
		EXPECT_EQ(
			FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(1920, 1080,
				EGroundTruthAmbientOcclusionQuality::FullResolution),
			4'147'200u);
		EXPECT_EQ(
			FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(1920, 1080,
				EGroundTruthAmbientOcclusionQuality::HalfResolution),
			3'628'800u);
		EXPECT_GE(FGroundTruthAmbientOcclusionRenderer::MaximumRetainedBytes,
			8u * FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
				1920, 1080,
				EGroundTruthAmbientOcclusionQuality::FullResolution));
		EXPECT_LT(FGroundTruthAmbientOcclusionRenderer::MaximumRetainedBytes,
			9u * FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
				1920, 1080,
				EGroundTruthAmbientOcclusionQuality::FullResolution));
	}

	TEST(FRendererRenderTargetLayoutTests, GroundTruthAmbientOcclusionHalfMappingAndSelectorAreExact)
	{
		using FRenderer = FGroundTruthAmbientOcclusionRenderer;
		EXPECT_EQ(FRenderer::CalculateHalfExtent(0), 0u);
		EXPECT_EQ(FRenderer::CalculateHalfExtent(1), 1u);
		EXPECT_EQ(FRenderer::CalculateHalfExtent(4), 2u);
		EXPECT_EQ(FRenderer::CalculateHalfExtent(5), 3u);
		EXPECT_EQ(FRenderer::MapFullRectangleToHalf({0, 0, 4, 6}),
			(FRenderer::FRectangle{0, 0, 2, 3}));
		EXPECT_EQ(FRenderer::MapFullRectangleToHalf({1, 3, 5, 7}),
			(FRenderer::FRectangle{0, 1, 3, 4}));
		EXPECT_EQ(FRenderer::MapFullRectangleToHalf({7, 9, 1, 1}),
			(FRenderer::FRectangle{3, 4, 1, 1}));

		uint32 LocalX = 99;
		uint32 LocalY = 99;
		for (uint32 Y = 0; Y < 2; ++Y)
		{
			for (uint32 X = 0; X < 2; ++X)
			{
				const uint8 Selector = FRenderer::EncodeSelector(X, Y);
				ASSERT_TRUE(FRenderer::DecodeSelector(Selector, LocalX, LocalY));
				EXPECT_EQ(LocalX, X);
				EXPECT_EQ(LocalY, Y);
			}
		}
		EXPECT_FALSE(FRenderer::DecodeSelector(
			FRenderer::InvalidSelector, LocalX, LocalY));
		EXPECT_EQ(FRenderer::EncodeSelector(2, 0), FRenderer::InvalidSelector);
		EXPECT_EQ(FRenderer::SelectRepresentative({{{false, 0.0f},
			{false, 0.0f}, {false, 0.0f}, {false, 0.0f}}}),
			FRenderer::InvalidSelector);
		EXPECT_EQ(FRenderer::SelectRepresentative({{{true, 4.0f},
			{true, 2.0f}, {true, 2.0f}, {true, 3.0f}}}), 2u);

	}

	TEST(FRendererRenderTargetLayoutTests, OutputVariantOwnsOnlyTheColorFinalTransition)
	{
		const FRHIRenderTargetLayout Offscreen = MakeEditorAssistanceOutput(EViewportOutput::Offscreen);
		const FRHIRenderTargetLayout Present = MakeEditorAssistanceOutput(EViewportOutput::Present);

		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::Present);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::Present);
		EXPECT_EQ(Offscreen.DepthStencilAttachment, Present.DepthStencilAttachment);
	}
} // namespace Durin
