#include "RHIResources.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHIResourceTransitionValidationTests, AccessFailuresStayInTransitionDomainAndRetainBatchLocation)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"Access", 64, 4, EBufferUsageFlags::DestinationCopy));
		FRHITexture Texture(FRHITextureCreateDesc::Create2D(
			"Access", 8, 8, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::DestinationCopy));
		for (const auto Access : {ERHIAccess::None, ERHIAccess::Discard,
			ERHIAccess::Discard | ERHIAccess::TransferWrite,
			ERHIAccess::TransferRead | ERHIAccess::TransferWrite,
			static_cast<ERHIAccess>(1u << 30)})
		{
			const auto BufferResult = ValidateBufferTransition(
				FRHIBufferTransition::Whole(&Buffer, ERHIAccess::Discard, Access));
			ASSERT_FALSE(BufferResult);
			EXPECT_EQ(BufferResult.error(), ERHIBufferTransitionError::InvalidAccess);
			const auto TextureResult = ValidateTextureTransition(
				FRHITextureTransition::Whole(&Texture, ERHIAccess::Discard, Access));
			ASSERT_FALSE(TextureResult);
			EXPECT_EQ(TextureResult.error(), ERHITextureTransitionError::InvalidAccess);
		}

		const std::array Transitions{
			FRHIBufferTransition{&Buffer, 0, 32, ERHIAccess::None, ERHIAccess::TransferWrite},
			FRHIBufferTransition{&Buffer, 32, 32, ERHIAccess::TransferWrite,
				ERHIAccess::TransferRead | ERHIAccess::TransferWrite}};
		const auto Batch = ValidateBufferTransitions(Transitions);
		ASSERT_FALSE(Batch);
		EXPECT_EQ(Batch.error().Code, ERHIBufferTransitionError::InvalidAccess);
		EXPECT_EQ(Batch.error().Index, 1u);
		EXPECT_FALSE(Batch.error().OtherIndex);
	}

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
		const auto BufferTransitionResult = ValidateBufferTransition(Whole);
		EXPECT_TRUE(BufferTransitionResult) << ToString(BufferTransitionResult.error());

		FRHIBufferTransition Invalid = Whole;
		Invalid.Size = 0;
		const auto BufferTransitionResult2 = ValidateBufferTransition(Invalid);
		ASSERT_FALSE(BufferTransitionResult2);
		Invalid = Whole;
		Invalid.Offset = std::numeric_limits<uint64>::max();
		Invalid.Size = 2;
		const auto BufferTransitionResult3 = ValidateBufferTransition(Invalid);
		ASSERT_FALSE(BufferTransitionResult3);
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::Discard;
		const auto BufferTransitionResult4 = ValidateBufferTransition(Invalid);
		ASSERT_FALSE(BufferTransitionResult4);
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::VertexBufferRead | ERHIAccess::TransferWrite;
		const auto BufferTransitionResult5 = ValidateBufferTransition(Invalid);
		ASSERT_FALSE(BufferTransitionResult5);
		Invalid = Whole;
		Invalid.RequiredAfter = ERHIAccess::ComputeShaderRead;
		const auto BufferTransitionResult6 = ValidateBufferTransition(Invalid);
		ASSERT_FALSE(BufferTransitionResult6);
	}

	TEST(FRHIResourceTransitionValidationTests, RejectsBufferOverlapButAllowsAdjacency)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::CreateVertex("Ranges", 64));
		std::array Transitions{
			FRHIBufferTransition{&Buffer, 0, 16, ERHIAccess::Discard, ERHIAccess::VertexBufferRead},
			FRHIBufferTransition{&Buffer, 16, 16, ERHIAccess::Discard, ERHIAccess::VertexBufferRead}};
		const auto BufferTransitionsResult = ValidateBufferTransitions(Transitions);
		EXPECT_TRUE(BufferTransitionsResult) << ToString(BufferTransitionsResult.error());
		Transitions[1].Offset = 15;
		const auto BufferTransitionsResult2 = ValidateBufferTransitions(Transitions);
		ASSERT_FALSE(BufferTransitionsResult2);
		EXPECT_EQ(BufferTransitionsResult2.error().Code, ERHIBufferTransitionError::OverlappingRanges);
		EXPECT_EQ(BufferTransitionsResult2.error().Index, 1u);
		EXPECT_EQ(BufferTransitionsResult2.error().OtherIndex, 0u);
		Transitions[1].Size = 0;
		const auto EmptyRange = ValidateBufferTransitions(Transitions);
		ASSERT_FALSE(EmptyRange);
		EXPECT_EQ(EmptyRange.error().Code, ERHIBufferTransitionError::EmptyRange);
		EXPECT_EQ(EmptyRange.error().Index, 1u);
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
		const auto TextureTransitionResult = ValidateTextureTransition(Whole);
		EXPECT_TRUE(TextureTransitionResult) << ToString(TextureTransitionResult.error());

		FRHITextureTransition Edge = Whole;
		Edge.Range = {ERHITextureAspect::Color, 3, 1, 3, 1};
		const auto TextureTransitionResult2 = ValidateTextureTransition(Edge);
		EXPECT_TRUE(TextureTransitionResult2) << ToString(TextureTransitionResult2.error());
		Edge.Range.NumMips = 2;
		const auto TextureTransitionResult3 = ValidateTextureTransition(Edge);
		ASSERT_FALSE(TextureTransitionResult3);
		Edge = Whole;
		Edge.Range.Aspects = ERHITextureAspect::Depth;
		const auto TextureTransitionResult4 = ValidateTextureTransition(Edge);
		ASSERT_FALSE(TextureTransitionResult4);
		Edge = Whole;
		Edge.RequiredAfter = ERHIAccess::GraphicsUniformRead;
		const auto TextureTransitionResult5 = ValidateTextureTransition(Edge);
		ASSERT_FALSE(TextureTransitionResult5);
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
		const auto TextureTransitionsResult = ValidateTextureTransitions(Transitions);
		EXPECT_TRUE(TextureTransitionsResult) << ToString(TextureTransitionsResult.error());
		Transitions[1].Range.Aspects = ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		const auto TextureTransitionsResult2 = ValidateTextureTransitions(Transitions);
		ASSERT_FALSE(TextureTransitionsResult2);
	}

	TEST(FRHIResourceTransitionValidationTests, RejectsNullResourcesAndEmptyRanges)
	{
		const auto BufferTransitionResult = ValidateBufferTransition(
			FRHIBufferTransition::Whole(nullptr, ERHIAccess::Discard, ERHIAccess::TransferWrite));
		ASSERT_FALSE(BufferTransitionResult);
		const auto TextureTransitionResult = ValidateTextureTransition(
			FRHITextureTransition::Whole(nullptr, ERHIAccess::Discard, ERHIAccess::TransferWrite));
		ASSERT_FALSE(TextureTransitionResult);
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
		const auto TextureTransitionResult = ValidateTextureTransition(FirstWrite);
		EXPECT_TRUE(TextureTransitionResult) << ToString(TextureTransitionResult.error());
		const auto TextureTransitionResult2 = ValidateTextureTransition(FirstRead);
		EXPECT_TRUE(TextureTransitionResult2) << ToString(TextureTransitionResult2.error());
		const auto TextureTransitionResult3 = ValidateTextureTransition(Rewrite);
		EXPECT_TRUE(TextureTransitionResult3) << ToString(TextureTransitionResult3.error());

		FRHITexture Invalid(FRHITextureCreateDesc::Create2D(
			"InvalidShadow", 16, 16, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		const auto TextureTransitionResult4 = ValidateTextureTransition(
			FRHITextureTransition::Whole(
				&Invalid, ERHIAccess::DepthStencilReadWrite,
				ERHIAccess::GraphicsShaderRead));
		ASSERT_FALSE(TextureTransitionResult4);
	}
} // namespace Durin
