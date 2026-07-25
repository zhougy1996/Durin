#include <gtest/gtest.h>

#include "ImageDecoder.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint8 TransparentPngBytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

		auto WriteFixture(std::string_view Name, std::span<const uint8> Bytes) -> std::filesystem::path
		{
			const std::filesystem::path Path = std::filesystem::path(DURIN_TEST_WORK_DIR) / Name;
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			return Path;
		}
	} // namespace

	TEST(FImageDecoderTests, RecognizesSupportedExtensionsCaseInsensitively)
	{
		EXPECT_TRUE(IsSupportedImageExtension(".PNG"));
		EXPECT_TRUE(IsSupportedImageExtension(".jpg"));
		EXPECT_TRUE(IsSupportedImageExtension(".jpeg"));
		EXPECT_TRUE(IsSupportedImageExtension(".bmp"));
		EXPECT_TRUE(IsSupportedImageExtension(".tga"));
		EXPECT_FALSE(IsSupportedImageExtension(".gif"));
		EXPECT_FALSE(IsSupportedImageExtension(".dasset"));
	}

	TEST(FImageDecoderTests, DecodesMemoryToUnscaledRgba8)
	{
		FDecodedImage Image;
		std::string Error;
		ASSERT_TRUE(DecodeImageFromMemory(TransparentPngBytes, Image, Error)) << Error;
		EXPECT_EQ(Image.Width, 2u);
		EXPECT_EQ(Image.Height, 1u);
		EXPECT_EQ(Image.SourceChannelCount, 4u);
		ASSERT_EQ(Image.Pixels.size(), 8u);
		EXPECT_EQ(Image.Pixels[0], 255u);
		EXPECT_EQ(Image.Pixels[1], 0u);
		EXPECT_EQ(Image.Pixels[2], 0u);
		EXPECT_EQ(Image.Pixels[3], 255u);
		EXPECT_TRUE(Image.bHasTransparency);
	}

	TEST(FImageDecoderTests, DecodesFileThroughSharedPath)
	{
		const std::filesystem::path Path = WriteFixture("AssetCoreTransparent.png", TransparentPngBytes);
		FDecodedImage Image;
		std::string Error;
		ASSERT_TRUE(DecodeImageFromFile(Path.generic_string(), Image, Error)) << Error;
		EXPECT_EQ(Image.Width, 2u);
		EXPECT_EQ(Image.Height, 1u);
		EXPECT_TRUE(Image.bHasTransparency);
	}

	TEST(FImageDecoderTests, RejectsEmptyAndCorruptDataWithoutPartialOutput)
	{
		FDecodedImage Image;
		Image.Pixels = {255};
		Image.Width = 1;
		std::string Error;
		EXPECT_FALSE(DecodeImageFromMemory({}, Image, Error));
		EXPECT_TRUE(Image.Pixels.empty());
		EXPECT_EQ(Image.Width, 0u);
		EXPECT_FALSE(Error.empty());

		constexpr uint8 CorruptBytes[] = {1, 2, 3, 4, 5};
		EXPECT_FALSE(DecodeImageFromMemory(CorruptBytes, Image, Error));
		EXPECT_TRUE(Image.Pixels.empty());
		EXPECT_FALSE(Error.empty());
	}

	TEST(FImageDecoderTests, RejectsImagesOutsideCallerLimitsBeforeDecoding)
	{
		FDecodedImage Image;
		std::string Error;
		FImageDecodeLimits Limits;
		Limits.MaximumEncodedBytes = 8;
		EXPECT_FALSE(DecodeImageFromMemory(TransparentPngBytes, Image, Error, Limits));
		EXPECT_EQ(Error, "The encoded image is too large.");

		std::vector<uint8> OversizedPng(std::begin(TransparentPngBytes), std::end(TransparentPngBytes));
		// The IHDR advertises 8192 x 8192 pixels; stbi_info reads it without allocating the decoded image.
		OversizedPng[16] = 0;
		OversizedPng[17] = 0;
		OversizedPng[18] = 32;
		OversizedPng[19] = 0;
		OversizedPng[20] = 0;
		OversizedPng[21] = 0;
		OversizedPng[22] = 32;
		OversizedPng[23] = 0;
		Limits.MaximumEncodedBytes = 32ull * 1024ull * 1024ull;
		Limits.MaximumDecodedPixels = 16ull * 1024ull * 1024ull;
		EXPECT_FALSE(DecodeImageFromMemory(OversizedPng, Image, Error, Limits));
		EXPECT_EQ(Error, "The decoded image is too large.");
		EXPECT_TRUE(Image.Pixels.empty());
	}
} // namespace Durin::Asset
