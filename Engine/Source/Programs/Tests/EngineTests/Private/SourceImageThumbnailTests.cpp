#include <gtest/gtest.h>

#include "SourceImageThumbnailDecoder.h"

namespace Durin
{
	namespace
	{
		auto WriteBinaryFixture(std::string_view Name, std::span<const uint8> Bytes) -> std::filesystem::path
		{
			const std::filesystem::path Path = std::filesystem::path(DURIN_TEST_WORK_DIR) / Name;
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			return Path;
		}
	}

	TEST(FSourceImageThumbnailTests, RecognizesOnlySupportedExtensionsCaseInsensitively)
	{
		EXPECT_TRUE(IsSupportedSourceImageExtension(".PNG"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".jpg"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".jpeg"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".bmp"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".tga"));
		EXPECT_FALSE(IsSupportedSourceImageExtension(".gif"));
		EXPECT_FALSE(IsSupportedSourceImageExtension(".dasset"));
	}

	TEST(FSourceImageThumbnailTests, DecodesTransparentPngAndPreservesAspectRatio)
	{
		// 2x1 RGBA PNG with one opaque red texel and one transparent texel.
		constexpr uint8 PngBytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
		const std::filesystem::path Path = WriteBinaryFixture("ThumbnailTransparent.png", PngBytes);
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		ASSERT_TRUE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error)) << Error;
		EXPECT_EQ(Thumbnail.Width, 2u);
		EXPECT_EQ(Thumbnail.Height, 1u);
		EXPECT_EQ(Thumbnail.Pixels.size(), 8u);
		EXPECT_TRUE(Thumbnail.bHasTransparency);
	}

	TEST(FSourceImageThumbnailTests, DecodesJpeg)
	{
		constexpr uint8 JpegBytes[] = {
			255, 216, 255, 224, 0, 16, 74, 70, 73, 70, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 255, 219, 0, 67, 0, 40, 28, 30, 35, 30, 25, 40,
			35, 33, 35, 45, 43, 40, 48, 60, 100, 65, 60, 55, 55, 60, 123, 88, 93, 73, 100, 145, 128, 153, 150, 143, 128, 140, 138, 160, 180, 230,
			195, 160, 170, 218, 173, 138, 140, 200, 255, 203, 218, 238, 245, 255, 255, 255, 155, 193, 255, 255, 255, 250, 255, 230, 253, 255, 248, 255,
			192, 0, 11, 8, 0, 1, 0, 1, 1, 1, 17, 0, 255, 196, 0, 20, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			255, 196, 0, 20, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 218, 0, 8, 1, 1, 0, 0, 63, 0, 63, 255, 217};
		const std::filesystem::path Path = WriteBinaryFixture("Thumbnail.jpg", JpegBytes);
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		ASSERT_TRUE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error)) << Error;
		EXPECT_EQ(Thumbnail.Width, 1u);
		EXPECT_EQ(Thumbnail.Height, 1u);
		EXPECT_FALSE(Thumbnail.bHasTransparency);
	}

	TEST(FSourceImageThumbnailTests, ShrinksLargeTgaToMaximumDimension)
	{
		constexpr uint32 Width = 512;
		constexpr uint32 Height = 256;
		std::vector<uint8> TgaBytes(18 + static_cast<size_t>(Width) * Height * 3, 0);
		TgaBytes[2] = 2;
		TgaBytes[12] = static_cast<uint8>(Width & 0xff);
		TgaBytes[13] = static_cast<uint8>(Width >> 8);
		TgaBytes[14] = static_cast<uint8>(Height & 0xff);
		TgaBytes[15] = static_cast<uint8>(Height >> 8);
		TgaBytes[16] = 24;
		TgaBytes[17] = 0x20;
		const std::filesystem::path Path = WriteBinaryFixture("ThumbnailLarge.tga", TgaBytes);
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		ASSERT_TRUE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error)) << Error;
		EXPECT_EQ(Thumbnail.Width, 256u);
		EXPECT_EQ(Thumbnail.Height, 128u);
		EXPECT_EQ(Thumbnail.Pixels.size(), 256u * 128u * 4u);
	}

	TEST(FSourceImageThumbnailTests, RejectsCorruptImage)
	{
		constexpr uint8 Bytes[] = {1, 2, 3, 4, 5};
		const std::filesystem::path Path = WriteBinaryFixture("ThumbnailCorrupt.png", Bytes);
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		EXPECT_FALSE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error));
		EXPECT_FALSE(Error.empty());
	}
} // namespace Durin
