#include "RHIResources.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHITransferValidationTests, FootprintResultDoesNotRetainPreviousFailure)
	{
		FRHITexture Texture(FRHITextureCreateDesc::Create2D("Footprint", 8, 8, EPixelFormat::RGBA8_UNORM));
		FRHIBufferTextureCopyRegion Region{.BufferOffset = 1, .TextureExtent = {8, 8, 1}};
		auto Result = GetBufferTextureCopyFootprint(Texture, Region);
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error(), ERHICopyFootprintError::OffsetAlignment);
		Region.BufferOffset = 0;
		Result = GetBufferTextureCopyFootprint(Texture, Region);
		ASSERT_TRUE(Result);
		EXPECT_EQ(*Result, 256u);
	}

	TEST(FRHITransferValidationTests, FootprintRejectsUndefinedArithmeticAndOverflow)
	{
		FRHITexture Texture(FRHITextureCreateDesc::Create2D("Footprint", 8, 8, EPixelFormat::RGBA8_UNORM));
		for (const auto Extent : {FRHITextureExtent3D{0, 8, 1}, FRHITextureExtent3D{8, 0, 1}})
		{
			const auto Result = GetBufferTextureCopyFootprint(Texture, {.TextureExtent = Extent});
			ASSERT_FALSE(Result);
			EXPECT_EQ(Result.error(), ERHICopyFootprintError::EmptyFootprint);
		}
		const auto EmptyLayers = GetBufferTextureCopyFootprint(Texture,
			{.TextureNumArrayLayers = 0, .TextureExtent = {8, 8, 1}});
		ASSERT_FALSE(EmptyLayers);
		EXPECT_EQ(EmptyLayers.error(), ERHICopyFootprintError::EmptyFootprint);
		FRHITexture Unknown(FRHITextureCreateDesc::Create2D("Unknown", 8, 8, EPixelFormat::Unknown));
		const auto InvalidFormat = GetBufferTextureCopyFootprint(Unknown, {.TextureExtent = {8, 8, 1}});
		ASSERT_FALSE(InvalidFormat);
		EXPECT_EQ(InvalidFormat.error(), ERHICopyFootprintError::InvalidBlockLayout);
		const auto Overflow = GetBufferTextureCopyFootprint(Texture,
			{.BufferRowLength = UINT32_MAX, .BufferImageHeight = UINT32_MAX, .TextureExtent = {1, 1, 1}});
		ASSERT_FALSE(Overflow);
		EXPECT_EQ(Overflow.error(), ERHICopyFootprintError::ImagePitchOverflow);
	}

	TEST(FRHITransferValidationTests, ValidatesBufferBoundsUsageAliasingAndDestinations)
	{
		FRHIBuffer Source(FRHIBufferCreateDesc::Create(
			"Source", 128, 4, EBufferUsageFlags::SourceCopy));
		FRHIBuffer Destination(FRHIBufferCreateDesc::Create(
			"Destination", 128, 4, EBufferUsageFlags::DestinationCopy));
		std::array Regions{
			FRHIBufferCopyRegion{0, 32, 16},
			FRHIBufferCopyRegion{16, 48, 16}};
		const auto BufferCopiesResult = ValidateBufferCopies(&Source, &Destination, Regions);
		EXPECT_TRUE(BufferCopiesResult) << FormatRHIError(BufferCopiesResult.error());
		Regions[1].DestinationOffset = 40;
		const auto BufferCopiesResult2 = ValidateBufferCopies(&Source, &Destination, Regions);
		ASSERT_FALSE(BufferCopiesResult2);
		EXPECT_EQ(BufferCopiesResult2.error().Code, ERHIBufferCopyError::OverlappingDestinations);
		EXPECT_EQ(BufferCopiesResult2.error().Index, 1u);
		EXPECT_EQ(BufferCopiesResult2.error().OtherIndex, 0u);
		Regions[1] = {120, 64, 16};
		const auto BufferCopiesResult3 = ValidateBufferCopies(&Source, &Destination, Regions);
		ASSERT_FALSE(BufferCopiesResult3);

		FRHIBuffer Aliased(FRHIBufferCreateDesc::Create(
			"Aliased", 128, 4,
			EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
		const auto BufferCopiesResult4 = ValidateBufferCopies(&Aliased, &Aliased,
			std::array{FRHIBufferCopyRegion{0, 8, 16}});
		ASSERT_FALSE(BufferCopiesResult4);
		const auto BufferCopiesResult5 = ValidateBufferCopies(&Aliased, &Aliased,
			std::array{FRHIBufferCopyRegion{0, 64, 16}});
		EXPECT_TRUE(BufferCopiesResult5) << FormatRHIError(BufferCopiesResult5.error());
	}

	TEST(FRHITransferValidationTests, ValidatesCompressedBufferTextureLayoutsAndEdges)
	{
		FRHIBuffer Source(FRHIBufferCreateDesc::Create(
			"CompressedSource", 256, 0, EBufferUsageFlags::SourceCopy));
		FRHIBuffer Destination(FRHIBufferCreateDesc::Create(
			"CompressedDestination", 256, 0, EBufferUsageFlags::DestinationCopy));
		FRHITexture Texture(FRHITextureCreateDesc::Create2D(
			"Compressed", 7, 7, EPixelFormat::BC1_UNORM)
			.SetFlags(ETextureCreateFlags::SourceCopy | ETextureCreateFlags::DestinationCopy));
		FRHIBufferTextureCopyRegion Region{
			.BufferOffset = 0,
			.TextureExtent = {7, 7, 1}};
		const auto BufferToTextureCopiesResult = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1));
		EXPECT_TRUE(BufferToTextureCopiesResult) << FormatRHIError(BufferToTextureCopiesResult.error());
		const auto TextureToBufferCopiesResult = ValidateTextureToBufferCopies(&Texture, &Destination,
			std::span(&Region, 1));
		EXPECT_TRUE(TextureToBufferCopiesResult) << FormatRHIError(TextureToBufferCopiesResult.error());

		Region.TextureOffset.X = 1;
		const auto BufferToTextureCopiesResult2 = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1));
		ASSERT_FALSE(BufferToTextureCopiesResult2);
		EXPECT_EQ(BufferToTextureCopiesResult2.error().Code,
			FRHIBufferTextureCopyError::FCode{ERHITextureCopyRegionError::BoxOutOfBounds});
		EXPECT_EQ(BufferToTextureCopiesResult2.error().Index, 0u);
		Region.TextureOffset.X = 0;
		Region.BufferRowLength = 7;
		const auto BufferToTextureCopiesResult3 = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1));
		ASSERT_FALSE(BufferToTextureCopiesResult3);
		EXPECT_EQ(BufferToTextureCopiesResult3.error().Code,
			FRHIBufferTextureCopyError::FCode{ERHICopyFootprintError::BlockAlignment});
		EXPECT_EQ(BufferToTextureCopiesResult3.error().Index, 0u);
		Region.BufferRowLength = 8;
		Region.BufferImageHeight = 8;
		const auto BufferToTextureCopiesResult4 = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1));
		EXPECT_TRUE(BufferToTextureCopiesResult4) << FormatRHIError(BufferToTextureCopiesResult4.error());
	}

	TEST(FRHITransferValidationTests, ValidatesExactTextureCopiesAndRejectsConversion)
	{
		FRHITexture Source(FRHITextureCreateDesc::CreateCube("Source")
			.SetExtent(8)
			.SetNumMips(2)
			.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::SourceCopy));
		FRHITexture Destination(FRHITextureCreateDesc::CreateCube("Destination")
			.SetExtent(8)
			.SetNumMips(2)
			.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::DestinationCopy));
		std::array Regions{
			FRHITextureCopyRegion{.SourceFirstArrayLayer = 2,
				.DestinationFirstArrayLayer = 4, .Extent = {8, 8, 1}},
			FRHITextureCopyRegion{.SourceMip = 1, .SourceFirstArrayLayer = 3,
				.DestinationMip = 1, .DestinationFirstArrayLayer = 5,
				.Extent = {4, 4, 1}}};
		const auto TextureCopiesResult = ValidateTextureCopies(&Source, &Destination, Regions);
		EXPECT_TRUE(TextureCopiesResult) << FormatRHIError(TextureCopiesResult.error());
		Regions[1].DestinationFirstArrayLayer = 4;
		Regions[1].DestinationMip = 0;
		const auto TextureCopiesResult2 = ValidateTextureCopies(&Source, &Destination, Regions);
		ASSERT_FALSE(TextureCopiesResult2);

		FRHITexture Different(FRHITextureCreateDesc::Create2D(
			"Different", 8, 8, EPixelFormat::BGRA8_UNORM)
			.SetFlags(ETextureCreateFlags::DestinationCopy));
		const auto TextureCopiesResult3 = ValidateTextureCopies(&Source, &Different,
			std::span(Regions).first(1));
		ASSERT_FALSE(TextureCopiesResult3);
	}

	TEST(FRHITransferValidationTests, RejectsMissingTransferIntent)
	{
		FRHIBuffer Source(FRHIBufferCreateDesc::Create(
			"Source", 16, 4, EBufferUsageFlags::None));
		FRHIBuffer Destination(FRHIBufferCreateDesc::Create(
			"Destination", 16, 4, EBufferUsageFlags::DestinationCopy));
		const auto BufferCopiesResult = ValidateBufferCopies(&Source, &Destination,
			std::span<const FRHIBufferCopyRegion>{});
		ASSERT_FALSE(BufferCopiesResult);
	}
} // namespace Durin
