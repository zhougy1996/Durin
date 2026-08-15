#include <gtest/gtest.h>

#include "Resources/RenderTargetLayouts.h"
#include "Renderers/PostProcessRenderer.h"

#include <limits>

namespace Durin
{
	using namespace RenderTargetLayouts;

	TEST(FRendererRenderTargetLayoutTests, SceneTargetsPreserveDepthForEditorAssistance)
	{
		const FRHIRenderTargetLayout Layout = MakeSceneTargets();

		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 2u);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.Format,
			EPixelFormat::RGBA16_FLOAT);
		EXPECT_EQ(Layout.ColorAttachments[1].RenderTarget.Format,
			EPixelFormat::R11G11B10_FLOAT);
		EXPECT_EQ(Layout.ColorAttachments[1].RenderTarget.FinalLayout,
			ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Layout.ColorAttachments[1].RenderTarget.FinalAccess,
			ERHIAccess::GraphicsShaderRead);
		ASSERT_TRUE(Layout.bHasDepthStencil);
		EXPECT_EQ(Layout.DepthStencilAttachment.Format, EPixelFormat::D32);
		EXPECT_EQ(Layout.DepthStencilAttachment.LoadAction, ERHIRenderTargetLoadAction::Clear);
		EXPECT_EQ(Layout.DepthStencilAttachment.StoreAction, ERHIRenderTargetStoreAction::Store);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalLayout, ERHITextureLayout::DepthStencilAttachment);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalAccess, ERHIAccess::DepthStencilReadWrite);
	}

	TEST(FRendererRenderTargetLayoutTests, ContactColorPreservesHDRSceneColor)
	{
		const FRHIRenderTargetLayout Layout = MakeContactShadowOutput();
		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 1u);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.Format,
			EPixelFormat::RGBA16_FLOAT);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalLayout,
			ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalAccess,
			ERHIAccess::GraphicsShaderRead);
	}

	TEST(FRendererRenderTargetLayoutTests, SceneTargetByteBudgetIsFormatAware)
	{
		EXPECT_EQ(FPostProcessRenderer::SceneTargetBytesPerPixel, 24u);
		EXPECT_EQ(
			FPostProcessRenderer::CalculateSceneTargetBytes(1920, 1080),
			49'766'400u);
		EXPECT_GE(
			FPostProcessRenderer::MaximumRetainedSceneTargetBytes,
			4u * FPostProcessRenderer::CalculateSceneTargetBytes(1920, 1080));
		EXPECT_LT(
			FPostProcessRenderer::MaximumRetainedSceneTargetBytes,
			5u * FPostProcessRenderer::CalculateSceneTargetBytes(1920, 1080));
		EXPECT_EQ(
			FPostProcessRenderer::CalculateSceneTargetBytes(
				std::numeric_limits<uint32>::max(),
				std::numeric_limits<uint32>::max()),
			std::numeric_limits<uint64>::max());
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

	TEST(FRendererRenderTargetLayoutTests,
		DirectionalShadowDepthPublishesStoredD32ForFragmentSampling)
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

		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalLayout,
			ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalAccess,
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalLayout,
			ERHITextureLayout::Present);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalAccess,
			ERHIAccess::Present);
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
