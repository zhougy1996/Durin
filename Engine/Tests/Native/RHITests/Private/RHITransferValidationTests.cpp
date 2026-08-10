#include "RHIResources.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHITransferValidationTests, ValidatesBufferBoundsUsageAliasingAndDestinations)
	{
		FRHIBuffer Source(FRHIBufferCreateDesc::Create(
			"Source", 128, 4, EBufferUsageFlags::SourceCopy));
		FRHIBuffer Destination(FRHIBufferCreateDesc::Create(
			"Destination", 128, 4, EBufferUsageFlags::DestinationCopy));
		std::array Regions{
			FRHIBufferCopyRegion{0, 32, 16},
			FRHIBufferCopyRegion{16, 48, 16}};
		std::string Error;
		EXPECT_TRUE(ValidateBufferCopies(&Source, &Destination, Regions, Error)) << Error;
		Regions[1].DestinationOffset = 40;
		EXPECT_FALSE(ValidateBufferCopies(&Source, &Destination, Regions, Error));
		Regions[1] = {120, 64, 16};
		EXPECT_FALSE(ValidateBufferCopies(&Source, &Destination, Regions, Error));

		FRHIBuffer Aliased(FRHIBufferCreateDesc::Create(
			"Aliased", 128, 4,
			EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
		EXPECT_FALSE(ValidateBufferCopies(&Aliased, &Aliased,
			std::array{FRHIBufferCopyRegion{0, 8, 16}}, Error));
		EXPECT_TRUE(ValidateBufferCopies(&Aliased, &Aliased,
			std::array{FRHIBufferCopyRegion{0, 64, 16}}, Error)) << Error;
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
		std::string Error;
		EXPECT_TRUE(ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1), Error)) << Error;
		EXPECT_TRUE(ValidateTextureToBufferCopies(&Texture, &Destination,
			std::span(&Region, 1), Error)) << Error;

		Region.TextureOffset.X = 1;
		EXPECT_FALSE(ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1), Error));
		Region.TextureOffset.X = 0;
		Region.BufferRowLength = 7;
		EXPECT_FALSE(ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1), Error));
		Region.BufferRowLength = 8;
		Region.BufferImageHeight = 8;
		EXPECT_TRUE(ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1), Error)) << Error;
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
		std::string Error;
		EXPECT_TRUE(ValidateTextureCopies(&Source, &Destination, Regions, Error)) << Error;
		Regions[1].DestinationFirstArrayLayer = 4;
		Regions[1].DestinationMip = 0;
		EXPECT_FALSE(ValidateTextureCopies(&Source, &Destination, Regions, Error));

		FRHITexture Different(FRHITextureCreateDesc::Create2D(
			"Different", 8, 8, EPixelFormat::BGRA8_UNORM)
			.SetFlags(ETextureCreateFlags::DestinationCopy));
		EXPECT_FALSE(ValidateTextureCopies(&Source, &Different,
			std::span(Regions).first(1), Error));
	}

	TEST(FRHITransferValidationTests, RejectsMissingTransferIntent)
	{
		FRHIBuffer Source(FRHIBufferCreateDesc::Create(
			"Source", 16, 4, EBufferUsageFlags::None));
		FRHIBuffer Destination(FRHIBufferCreateDesc::Create(
			"Destination", 16, 4, EBufferUsageFlags::DestinationCopy));
		std::string Error;
		EXPECT_FALSE(ValidateBufferCopies(&Source, &Destination,
			std::span<const FRHIBufferCopyRegion>{}, Error));
	}
} // namespace Durin
