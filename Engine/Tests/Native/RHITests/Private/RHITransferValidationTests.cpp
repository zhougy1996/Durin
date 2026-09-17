#include "RHIResources.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHITransferValidationTests, FootprintResultDoesNotRetainPreviousFailure)
	{
		FRHITexture Texture(FRHITextureCreateDesc::Create2D("Footprint", 8, 8, EPixelFormat::RGBA8_UNORM));
		FRHIBufferTextureCopyRegion Region{.BufferOffset = 1, .TextureExtent = {8, 8, 1}};
		uint64 Footprint = 123;
		auto Result = GetBufferTextureCopyFootprint(Texture, Region, Footprint);
		EXPECT_EQ(Result.Error.Code, FRHIError::FCode{ERHICopyFootprintError::OffsetAlignment});
		EXPECT_EQ(Footprint, 123u);
		Region.BufferOffset = 0;
		Result = GetBufferTextureCopyFootprint(Texture, Region, Footprint);
		EXPECT_TRUE(Result);
		EXPECT_FALSE(Result.Error.HasError());
		EXPECT_EQ(Footprint, 256u);
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
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateBufferCopies(&Source, &Destination, Regions))) << FormatRHIError(Error.Error);
		Regions[1].DestinationOffset = 40;
		EXPECT_FALSE((Error = ValidateBufferCopies(&Source, &Destination, Regions)));
		EXPECT_EQ(Error.Error.Code, FRHIError::FCode{ERHIBufferCopyError::OverlappingDestinations});
		EXPECT_EQ(Error.Error.Index, 1u);
		EXPECT_EQ(Error.Error.OtherIndex, 0u);
		Regions[1] = {120, 64, 16};
		EXPECT_FALSE((Error = ValidateBufferCopies(&Source, &Destination, Regions)));

		FRHIBuffer Aliased(FRHIBufferCreateDesc::Create(
			"Aliased", 128, 4,
			EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
		EXPECT_FALSE((Error = ValidateBufferCopies(&Aliased, &Aliased,
			std::array{FRHIBufferCopyRegion{0, 8, 16}})));
		EXPECT_TRUE((Error = ValidateBufferCopies(&Aliased, &Aliased,
			std::array{FRHIBufferCopyRegion{0, 64, 16}}))) << FormatRHIError(Error.Error);
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
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1)))) << FormatRHIError(Error.Error);
		EXPECT_TRUE((Error = ValidateTextureToBufferCopies(&Texture, &Destination,
			std::span(&Region, 1)))) << FormatRHIError(Error.Error);

		Region.TextureOffset.X = 1;
		EXPECT_FALSE((Error = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1))));
		Region.TextureOffset.X = 0;
		Region.BufferRowLength = 7;
		EXPECT_FALSE((Error = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1))));
		Region.BufferRowLength = 8;
		Region.BufferImageHeight = 8;
		EXPECT_TRUE((Error = ValidateBufferToTextureCopies(&Source, &Texture,
			std::span(&Region, 1)))) << FormatRHIError(Error.Error);
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
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateTextureCopies(&Source, &Destination, Regions))) << FormatRHIError(Error.Error);
		Regions[1].DestinationFirstArrayLayer = 4;
		Regions[1].DestinationMip = 0;
		EXPECT_FALSE((Error = ValidateTextureCopies(&Source, &Destination, Regions)));

		FRHITexture Different(FRHITextureCreateDesc::Create2D(
			"Different", 8, 8, EPixelFormat::BGRA8_UNORM)
			.SetFlags(ETextureCreateFlags::DestinationCopy));
		EXPECT_FALSE((Error = ValidateTextureCopies(&Source, &Different,
			std::span(Regions).first(1))));
	}

	TEST(FRHITransferValidationTests, RejectsMissingTransferIntent)
	{
		FRHIBuffer Source(FRHIBufferCreateDesc::Create(
			"Source", 16, 4, EBufferUsageFlags::None));
		FRHIBuffer Destination(FRHIBufferCreateDesc::Create(
			"Destination", 16, 4, EBufferUsageFlags::DestinationCopy));
		FRHIOperationResult Error;
		EXPECT_FALSE((Error = ValidateBufferCopies(&Source, &Destination,
			std::span<const FRHIBufferCopyRegion>{})));
	}
} // namespace Durin
