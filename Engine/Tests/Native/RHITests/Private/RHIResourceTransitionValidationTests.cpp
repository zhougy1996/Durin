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
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateBufferTransition(Whole))) << FormatRHIError(Error.Error);

		FRHIBufferTransition Invalid = Whole;
		Invalid.Size = 0;
		EXPECT_FALSE((Error = ValidateBufferTransition(Invalid)));
		Invalid = Whole;
		Invalid.Offset = std::numeric_limits<uint64>::max();
		Invalid.Size = 2;
		EXPECT_FALSE((Error = ValidateBufferTransition(Invalid)));
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::Discard;
		EXPECT_FALSE((Error = ValidateBufferTransition(Invalid)));
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::VertexBufferRead | ERHIAccess::TransferWrite;
		EXPECT_FALSE((Error = ValidateBufferTransition(Invalid)));
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::ComputeShaderRead;
		EXPECT_FALSE((Error = ValidateBufferTransition(Invalid)));
	}

	TEST(FRHIResourceTransitionValidationTests, RejectsBufferOverlapButAllowsAdjacency)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::CreateVertex("Ranges", 64));
		std::array Transitions{
			FRHIBufferTransition{&Buffer, 0, 16, ERHIAccess::Discard, ERHIAccess::VertexBufferRead},
			FRHIBufferTransition{&Buffer, 16, 16, ERHIAccess::Discard, ERHIAccess::VertexBufferRead}};
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateBufferTransitions(Transitions))) << FormatRHIError(Error.Error);
		Transitions[1].Offset = 15;
		EXPECT_FALSE((Error = ValidateBufferTransitions(Transitions)));
		EXPECT_EQ(Error.Error.Code, FRHIError::FCode{ERHIBufferTransitionError::OverlappingRanges});
		EXPECT_EQ(Error.Error.Index, 1u);
		EXPECT_EQ(Error.Error.OtherIndex, 0u);
		Transitions[1].Size = 0;
		Error = ValidateBufferTransitions(Transitions);
		EXPECT_EQ(Error.Error.Code, FRHIError::FCode{ERHIBufferTransitionError::EmptyRange});
		EXPECT_EQ(Error.Error.Index, 1u);
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
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateTextureTransition(Whole))) << FormatRHIError(Error.Error);

		FRHITextureTransition Edge = Whole;
		Edge.Range = {ERHITextureAspect::Color, 3, 1, 3, 1};
		EXPECT_TRUE((Error = ValidateTextureTransition(Edge))) << FormatRHIError(Error.Error);
		Edge.Range.NumMips = 2;
		EXPECT_FALSE((Error = ValidateTextureTransition(Edge)));
		Edge = Whole;
		Edge.Range.Aspects = ERHITextureAspect::Depth;
		EXPECT_FALSE((Error = ValidateTextureTransition(Edge)));
		Edge = Whole;
		Edge.RequiredAfter = ERHIAccess::GraphicsUniformRead;
		EXPECT_FALSE((Error = ValidateTextureTransition(Edge)));
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
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateTextureTransitions(Transitions))) << FormatRHIError(Error.Error);
		Transitions[1].Range.Aspects = ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		EXPECT_FALSE((Error = ValidateTextureTransitions(Transitions)));
	}

	TEST(FRHIResourceTransitionValidationTests, RejectsNullResourcesAndEmptyRanges)
	{
		FRHIOperationResult Error;
		EXPECT_FALSE((Error = ValidateBufferTransition(
			FRHIBufferTransition::Whole(nullptr, ERHIAccess::Discard, ERHIAccess::TransferWrite))));
		EXPECT_FALSE((Error = ValidateTextureTransition(
			FRHITextureTransition::Whole(nullptr, ERHIAccess::Discard, ERHIAccess::TransferWrite))));
	}

	TEST(FRHIResourceTransitionValidationTests,
		DirectionalShadowDepthSupportsRepeatedAttachmentAndComparisonReadCycles)
	{
		FRHITexture Shadow(FRHITextureCreateDesc::Create2DArray(
			"DirectionalShadowArray")
			.SetExtent(2048, 2048)
			.SetArraySize(3)
			.SetFormat(EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable
				| ETextureCreateFlags::ShaderResource));
		FRHIOperationResult Error;
		const FRHITextureTransition FirstWrite{
			&Shadow, {ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERHIAccess::Discard, ERHIAccess::DepthStencilReadWrite};
		const FRHITextureTransition FirstRead = FRHITextureTransition::Whole(
			&Shadow, ERHIAccess::DepthStencilReadWrite,
			ERHIAccess::GraphicsShaderRead);
		const FRHITextureTransition Rewrite{
			&Shadow, {ERHITextureAspect::Depth, 0, 1, 2, 1},
			ERHIAccess::GraphicsShaderRead,
			ERHIAccess::DepthStencilReadWrite};
		EXPECT_TRUE((Error = ValidateTextureTransition(FirstWrite))) << FormatRHIError(Error.Error);
		EXPECT_TRUE((Error = ValidateTextureTransition(FirstRead))) << FormatRHIError(Error.Error);
		EXPECT_TRUE((Error = ValidateTextureTransition(Rewrite))) << FormatRHIError(Error.Error);

		FRHITexture Invalid(FRHITextureCreateDesc::Create2D(
			"InvalidShadow", 16, 16, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		EXPECT_FALSE((Error = ValidateTextureTransition(
			FRHITextureTransition::Whole(
				&Invalid, ERHIAccess::DepthStencilReadWrite,
				ERHIAccess::GraphicsShaderRead))));
	}
} // namespace Durin
