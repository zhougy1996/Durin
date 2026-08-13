#include "RHIResources.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHIResourceTransitionValidationTests, MapsEveryTextureAccessToOnePortableLayout)
	{
		ERHITextureLayout Layout = ERHITextureLayout::Undefined;
		EXPECT_TRUE(GetTextureLayoutForAccess(ERHIAccess::GraphicsShaderRead, Layout));
		EXPECT_EQ(Layout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_TRUE(GetTextureLayoutForAccess(
			ERHIAccess::GraphicsShaderRead | ERHIAccess::ComputeShaderRead, Layout));
		EXPECT_EQ(Layout, ERHITextureLayout::ShaderReadOnly);
		EXPECT_TRUE(GetTextureLayoutForAccess(ERHIAccess::TransferRead, Layout));
		EXPECT_EQ(Layout, ERHITextureLayout::TransferSource);
		EXPECT_TRUE(GetTextureLayoutForAccess(ERHIAccess::TransferWrite, Layout));
		EXPECT_EQ(Layout, ERHITextureLayout::TransferDestination);
		EXPECT_TRUE(GetTextureLayoutForAccess(ERHIAccess::ComputeShaderReadWrite, Layout));
		EXPECT_EQ(Layout, ERHITextureLayout::General);
		EXPECT_FALSE(GetTextureLayoutForAccess(ERHIAccess::VertexBufferRead, Layout));
		EXPECT_FALSE(GetTextureLayoutForAccess(
			ERHIAccess::GraphicsShaderRead | ERHIAccess::TransferRead, Layout));
	}

	TEST(FRHIResourceTransitionValidationTests, ValidatesBufferStatesRangesAndWholeHelper)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"CombinedReadBuffer", 256, 16,
			EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer
				| EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::SourceCopy));
		const ERHIAccess CombinedRead = ERHIAccess::VertexBufferRead
			| ERHIAccess::GraphicsUniformRead | ERHIAccess::TransferRead;
		const FRHIBufferTransition Whole = FRHIBufferTransition::Whole(
			&Buffer, ERHIAccess::Discard, CombinedRead);
		EXPECT_EQ(Whole.Offset, 0u);
		EXPECT_EQ(Whole.Size, 256u);
		std::string Error;
		EXPECT_TRUE(ValidateBufferTransition(Whole, Error)) << Error;

		FRHIBufferTransition Invalid = Whole;
		Invalid.Size = 0;
		EXPECT_FALSE(ValidateBufferTransition(Invalid, Error));
		Invalid = Whole;
		Invalid.Offset = std::numeric_limits<uint64>::max();
		Invalid.Size = 2;
		EXPECT_FALSE(ValidateBufferTransition(Invalid, Error));
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::Discard;
		EXPECT_FALSE(ValidateBufferTransition(Invalid, Error));
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::VertexBufferRead | ERHIAccess::TransferWrite;
		EXPECT_FALSE(ValidateBufferTransition(Invalid, Error));
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::ComputeShaderRead;
		EXPECT_FALSE(ValidateBufferTransition(Invalid, Error));
	}

	TEST(FRHIResourceTransitionValidationTests, RejectsBufferOverlapButAllowsAdjacency)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::CreateVertex("Ranges", 64));
		std::array Transitions{
			FRHIBufferTransition{&Buffer, 0, 16, ERHIAccess::Discard, ERHIAccess::VertexBufferRead},
			FRHIBufferTransition{&Buffer, 16, 16, ERHIAccess::Discard, ERHIAccess::VertexBufferRead}};
		std::string Error;
		EXPECT_TRUE(ValidateBufferTransitions(Transitions, Error)) << Error;
		Transitions[1].Offset = 15;
		EXPECT_FALSE(ValidateBufferTransitions(Transitions, Error));
	}

	TEST(FRHIResourceTransitionValidationTests, ValidatesTextureAspectsEdgesAndWholeHelper)
	{
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2DArray("ColorArray")
			.SetExtent(64, 64)
			.SetArraySize(4)
			.SetNumMips(4)
			.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
		FRHITexture Texture(Desc);
		const FRHITextureTransition Whole = FRHITextureTransition::Whole(
			&Texture, ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Whole.Range, (FRHITextureSubresourceRange{
			ERHITextureAspect::Color, 0, 4, 0, 4}));
		std::string Error;
		EXPECT_TRUE(ValidateTextureTransition(Whole, Error)) << Error;

		FRHITextureTransition Edge = Whole;
		Edge.Range = {ERHITextureAspect::Color, 3, 1, 3, 1};
		EXPECT_TRUE(ValidateTextureTransition(Edge, Error)) << Error;
		Edge.Range.NumMips = 2;
		EXPECT_FALSE(ValidateTextureTransition(Edge, Error));
		Edge = Whole;
		Edge.Range.Aspects = ERHITextureAspect::Depth;
		EXPECT_FALSE(ValidateTextureTransition(Edge, Error));
		Edge = Whole;
		Edge.RequiredAfter = ERHIAccess::GraphicsUniformRead;
		EXPECT_FALSE(ValidateTextureTransition(Edge, Error));
	}

	TEST(FRHIResourceTransitionValidationTests, SeparatesDepthAndStencilSubresources)
	{
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
			"DepthStencil", 32, 32, EPixelFormat::D24S8)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable);
		FRHITexture Texture(Desc);
		EXPECT_EQ(GetTextureAspects(Texture.GetFormat()),
			ERHITextureAspect::Depth | ERHITextureAspect::Stencil);
		std::array Transitions{
			FRHITextureTransition{&Texture, {ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERHIAccess::Discard, ERHIAccess::DepthStencilReadWrite},
			FRHITextureTransition{&Texture, {ERHITextureAspect::Stencil, 0, 1, 0, 1},
				ERHIAccess::Discard, ERHIAccess::DepthStencilReadWrite}};
		std::string Error;
		EXPECT_TRUE(ValidateTextureTransitions(Transitions, Error)) << Error;
		Transitions[1].Range.Aspects = ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		EXPECT_FALSE(ValidateTextureTransitions(Transitions, Error));
	}

	TEST(FRHIResourceTransitionValidationTests, RejectsNullResourcesAndEmptyRanges)
	{
		std::string Error;
		EXPECT_FALSE(ValidateBufferTransition(
			FRHIBufferTransition::Whole(nullptr, ERHIAccess::Discard, ERHIAccess::TransferWrite), Error));
		EXPECT_FALSE(ValidateTextureTransition(
			FRHITextureTransition::Whole(nullptr, ERHIAccess::Discard, ERHIAccess::TransferWrite), Error));
	}

	TEST(FRHIResourceTransitionValidationTests,
		DirectionalShadowDepthSupportsRepeatedAttachmentAndComparisonReadCycles)
	{
		FRHITexture Shadow(FRHITextureCreateDesc::Create2D(
			"DirectionalShadow", 2048, 2048, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable
				| ETextureCreateFlags::ShaderResource));
		std::string Error;
		const FRHITextureTransition FirstWrite = FRHITextureTransition::Whole(
			&Shadow, ERHIAccess::Discard, ERHIAccess::DepthStencilReadWrite);
		const FRHITextureTransition FirstRead = FRHITextureTransition::Whole(
			&Shadow, ERHIAccess::DepthStencilReadWrite,
			ERHIAccess::GraphicsShaderRead);
		const FRHITextureTransition Rewrite = FRHITextureTransition::Whole(
			&Shadow, ERHIAccess::GraphicsShaderRead,
			ERHIAccess::DepthStencilReadWrite);
		EXPECT_TRUE(ValidateTextureTransition(FirstWrite, Error)) << Error;
		EXPECT_TRUE(ValidateTextureTransition(FirstRead, Error)) << Error;
		EXPECT_TRUE(ValidateTextureTransition(Rewrite, Error)) << Error;

		FRHITexture Invalid(FRHITextureCreateDesc::Create2D(
			"InvalidShadow", 16, 16, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		EXPECT_FALSE(ValidateTextureTransition(
			FRHITextureTransition::Whole(
				&Invalid, ERHIAccess::DepthStencilReadWrite,
				ERHIAccess::GraphicsShaderRead),
			Error));
	}
} // namespace Durin
