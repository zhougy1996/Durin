#include <gtest/gtest.h>

#include "RHIResources.h"

namespace Durin
{
	namespace
	{
		auto MakeAttachment(EPixelFormat Format, uint8 NumSamples = 1) -> FRHIAttachmentLayout
		{
			FRHIAttachmentLayout Attachment;
			Attachment.Format = Format;
			Attachment.NumSamples = NumSamples;
			Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			Attachment.FinalAccess = ERHIAccess::ShaderRead;
			return Attachment;
		}

		auto MakeLayout() -> FRHIRenderTargetLayout
		{
			FRHIRenderTargetLayout Layout;
			Layout.NumColorRenderTargets = 2;
			Layout.ColorAttachments[0].RenderTarget = MakeAttachment(EPixelFormat::RGBA8_UNORM, 4);
			Layout.ColorAttachments[0].bHasResolveTarget = true;
			Layout.ColorAttachments[0].ResolveTarget = MakeAttachment(EPixelFormat::RGBA8_UNORM);
			Layout.ColorAttachments[1].RenderTarget = MakeAttachment(EPixelFormat::RGBA16_FLOAT, 4);
			Layout.bHasDepthStencil = true;
			Layout.DepthStencilAttachment = MakeAttachment(EPixelFormat::D32, 4);
			Layout.DepthStencilAttachment.FinalLayout = ERHITextureLayout::DepthStencilAttachment;
			Layout.DepthStencilAttachment.FinalAccess = ERHIAccess::DepthStencilReadWrite;
			return Layout;
		}
	}

	TEST(FRenderTargetLayoutTests, EqualLayoutsHaveStableHashes)
	{
		const FRHIRenderTargetLayout First = MakeLayout();
		const FRHIRenderTargetLayout Second = MakeLayout();
		EXPECT_EQ(First, Second);
		EXPECT_EQ(FRHIRenderTargetLayoutHasher{}(First), FRHIRenderTargetLayoutHasher{}(Second));
	}

	TEST(FRenderTargetLayoutTests, CompatibilityFieldsChangeIdentity)
	{
		const FRHIRenderTargetLayout Baseline = MakeLayout();
		const size_t BaselineHash = FRHIRenderTargetLayoutHasher{}(Baseline);
		auto ExpectDifferent = [&Baseline, BaselineHash](FRHIRenderTargetLayout Candidate) {
			EXPECT_NE(Baseline, Candidate);
			EXPECT_NE(BaselineHash, FRHIRenderTargetLayoutHasher{}(Candidate));
		};

		auto Candidate = Baseline;
		Candidate.ColorAttachments[0].RenderTarget.Format = EPixelFormat::BGRA8_UNORM;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.ColorAttachments[0].RenderTarget.NumSamples = 8;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.ColorAttachments[0].RenderTarget.LoadAction = ERHIRenderTargetLoadAction::Load;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.ColorAttachments[0].RenderTarget.StoreAction = ERHIRenderTargetStoreAction::DontCare;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.ColorAttachments[0].RenderTarget.FinalLayout = ERHITextureLayout::Present;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.ColorAttachments[0].RenderTarget.FinalAccess = ERHIAccess::Present;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.NumColorRenderTargets = 1;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.ColorAttachments[0].bHasResolveTarget = false;
		ExpectDifferent(Candidate);
		Candidate = Baseline;
		Candidate.bHasDepthStencil = false;
		ExpectDifferent(Candidate);
	}

	TEST(FRenderTargetLayoutTests, ValidatesMrtDepthAndResolveConstraints)
	{
		EXPECT_TRUE(MakeLayout().IsValid());

		auto Layout = MakeLayout();
		Layout.ColorAttachments[1].RenderTarget.NumSamples = 1;
		EXPECT_FALSE(Layout.IsValid());
		Layout = MakeLayout();
		Layout.DepthStencilAttachment.NumSamples = 1;
		EXPECT_FALSE(Layout.IsValid());
		Layout = MakeLayout();
		Layout.ColorAttachments[0].ResolveTarget.NumSamples = 4;
		EXPECT_FALSE(Layout.IsValid());
		Layout = MakeLayout();
		Layout.ColorAttachments[0].ResolveTarget.Format = EPixelFormat::BGRA8_UNORM;
		EXPECT_FALSE(Layout.IsValid());
		Layout = MakeLayout();
		Layout.ColorAttachments[0].RenderTarget.LoadAction = ERHIRenderTargetLoadAction::Load;
		EXPECT_FALSE(Layout.IsValid());
		Layout = {};
		EXPECT_FALSE(Layout.IsValid());
	}
} // namespace Durin
