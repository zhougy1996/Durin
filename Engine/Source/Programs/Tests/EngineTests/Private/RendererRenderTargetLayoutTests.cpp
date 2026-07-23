#include <gtest/gtest.h>

#include "RendererRenderTargetLayouts.h"

namespace Durin
{
	using namespace RendererRenderTargetLayouts;

	TEST(FRendererRenderTargetLayoutTests, SceneTargetsPreserveDepthForEditorAssistance)
	{
		const FRHIRenderTargetLayout Layout = MakeSceneTargets();

		ASSERT_TRUE(Layout.IsValid());
		ASSERT_TRUE(Layout.bHasDepthStencil);
		EXPECT_EQ(Layout.DepthStencilAttachment.Format, EPixelFormat::D32);
		EXPECT_EQ(Layout.DepthStencilAttachment.LoadAction, ERHIRenderTargetLoadAction::Clear);
		EXPECT_EQ(Layout.DepthStencilAttachment.StoreAction, ERHIRenderTargetStoreAction::Store);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalLayout, ERHITextureLayout::DepthStencilAttachment);
		EXPECT_EQ(Layout.DepthStencilAttachment.FinalAccess, ERHIAccess::DepthStencilReadWrite);
	}

	TEST(FRendererRenderTargetLayoutTests, ScenePostProcessLeavesColorReadyForEditorAssistance)
	{
		const FRHIRenderTargetLayout Layout = MakeScenePostProcessOutput();

		ASSERT_TRUE(Layout.IsValid());
		ASSERT_EQ(Layout.NumColorRenderTargets, 1);
		EXPECT_FALSE(Layout.bHasDepthStencil);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::ColorAttachment);
		EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::ColorAttachmentWrite);
	}

	TEST(FRendererRenderTargetLayoutTests, FinalEditorAssistanceLoadsPreservedColorAndDepth)
	{
		for (const EViewportOutput Output : {EViewportOutput::Offscreen, EViewportOutput::Present})
		{
			const FRHIRenderTargetLayout Layout = MakeFinalEditorAssistanceOutput(Output);

			ASSERT_TRUE(Layout.IsValid());
			ASSERT_EQ(Layout.NumColorRenderTargets, 1);
			ASSERT_TRUE(Layout.bHasDepthStencil);
			EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.LoadAction, ERHIRenderTargetLoadAction::Load);
			EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.InitialLayout, ERHITextureLayout::ColorAttachment);
			EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.InitialAccess, ERHIAccess::ColorAttachmentWrite);
			EXPECT_EQ(Layout.DepthStencilAttachment.Format, EPixelFormat::D32);
			EXPECT_EQ(Layout.DepthStencilAttachment.LoadAction, ERHIRenderTargetLoadAction::Load);
			EXPECT_EQ(Layout.DepthStencilAttachment.StoreAction, ERHIRenderTargetStoreAction::DontCare);
			EXPECT_EQ(Layout.DepthStencilAttachment.InitialLayout, ERHITextureLayout::DepthStencilAttachment);
			EXPECT_EQ(Layout.DepthStencilAttachment.InitialAccess, ERHIAccess::DepthStencilReadWrite);
		}
	}

	TEST(FRendererRenderTargetLayoutTests, FinalOutputVariantOwnsOnlyTheColorFinalTransition)
	{
		const FRHIRenderTargetLayout Offscreen = MakeFinalEditorAssistanceOutput(EViewportOutput::Offscreen);
		const FRHIRenderTargetLayout Present = MakeFinalEditorAssistanceOutput(EViewportOutput::Present);

		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::ShaderRead);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::Present);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::Present);
		EXPECT_EQ(Offscreen.DepthStencilAttachment, Present.DepthStencilAttachment);
	}
} // namespace Durin
