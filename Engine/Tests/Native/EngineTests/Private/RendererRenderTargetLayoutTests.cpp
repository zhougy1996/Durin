#include <gtest/gtest.h>

#include "Resources/RenderTargetLayouts.h"

namespace Durin
{
	using namespace RenderTargetLayouts;

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

	TEST(FRendererRenderTargetLayoutTests, FinalScenePostProcessOwnsOutputTransition)
	{
		const FRHIRenderTargetLayout Offscreen =
			MakeFinalScenePostProcessOutput(EViewportOutput::Offscreen);
		const FRHIRenderTargetLayout Present =
			MakeFinalScenePostProcessOutput(EViewportOutput::Present);

		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalLayout,
			ERHITextureLayout::ShaderReadOnly);
		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalAccess,
			ERHIAccess::ShaderRead);
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
			EXPECT_EQ(Layout.ColorAttachments[0].RenderTarget.InitialAccess, ERHIAccess::ColorAttachmentWrite);
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
		EXPECT_EQ(Offscreen.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::ShaderRead);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalLayout, ERHITextureLayout::Present);
		EXPECT_EQ(Present.ColorAttachments[0].RenderTarget.FinalAccess, ERHIAccess::Present);
		EXPECT_EQ(Offscreen.DepthStencilAttachment, Present.DepthStencilAttachment);
	}
} // namespace Durin
