#include <gtest/gtest.h>

#include "Image/ImageDecoder.h"
#include "Image/ImageEncoder.h"
#include "NativeTestSupport.h"

namespace Durin::Image
{
	namespace
	{
		constexpr uint8 TransparentPngBytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

		auto WriteFixture(std::string_view Name, std::span<const std::byte> Bytes) -> std::filesystem::path
		{
			const std::filesystem::path Path = Durin::Testing::GetTestWorkDirectory() / Name;
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			return Path;
		}

		auto MakeOldRadianceFixture() -> Durin::FByteArray
		{
			constexpr std::string_view Header =
				"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
			const auto HeaderBytes = std::as_bytes(std::span{Header});
			Durin::FByteArray Result(HeaderBytes.begin(), HeaderBytes.end());
			Result.insert(Result.end(), {std::byte{128}, std::byte{64}, std::byte{32}, std::byte{131},
				std::byte{32}, std::byte{64}, std::byte{16}, std::byte{130}});
			return Result;
		}

		auto MakeNewRadianceFixture() -> Durin::FByteArray
		{
			constexpr std::string_view Header =
				"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n";
			const auto HeaderBytes = std::as_bytes(std::span{Header});
			Durin::FByteArray Result(HeaderBytes.begin(), HeaderBytes.end());
			Result.insert(Result.end(), {std::byte{2}, std::byte{2}, std::byte{0}, std::byte{8}});
			for (uint8 Value : {32, 64, 128, 131})
				Result.insert(Result.end(), {std::byte{128 + 8}, static_cast<std::byte>(Value)});
			return Result;
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
		EXPECT_FALSE(IsSupportedImageExtension(".hdr"));
		EXPECT_FALSE(IsSupportedImageExtension(".dasset"));
		EXPECT_TRUE(IsRadianceHDRExtension(".HDR"));
		EXPECT_FALSE(IsRadianceHDRExtension(".png"));
	}

	TEST(FImageTests, ValidatesSizesAndKeepsViewStorageAlive)
	{
		FImageInfo Info{.Width = 2, .Height = 1,
			.Format = ERawImageFormat::RGBA8,
			.GammaSpace = EImageGammaSpace::SRGB};
		uint64 ByteSize = 0;
		ASSERT_TRUE(Info.GetByteSize(ByteSize));
		EXPECT_EQ(ByteSize, 8u);
		FImage Image;
		std::string Error;
		ASSERT_TRUE(FImage::TryCreate(Info, FByteArray(8, std::byte{7}), Image, &Error)) << Error;
		const FImageView View = Image.GetView();
		Image.Reset();
		ASSERT_TRUE(View.IsValid());
		EXPECT_EQ(View.GetPixels()[0], std::byte{7});

		Info.Width = std::numeric_limits<uint32>::max();
		Info.Height = std::numeric_limits<uint32>::max();
		EXPECT_FALSE(Info.GetByteSize(ByteSize));
	}

	TEST(FImageTests, ConvertsGammaAndPreservesGrayscale16Samples)
	{
		FDecodedGrayscale16Image Gray{
			.Samples = {0x0000u, 0x1234u, 0xffffu}, .Width = 3, .Height = 1};
		FImage GrayImage;
		std::string Error;
		ASSERT_TRUE(Gray.ToImage(EImageGammaSpace::Linear, GrayImage, &Error)) << Error;
		EXPECT_EQ(GrayImage.GetInfo().Format, ERawImageFormat::G16);
		ASSERT_EQ(GrayImage.GetPixels().size(), Gray.Samples.size() * sizeof(uint16));
		EXPECT_EQ(std::memcmp(GrayImage.GetPixels().data(), Gray.Samples.data(),
			GrayImage.GetPixels().size()), 0);

		FDecodedImage Encoded{.Pixels = {std::byte{128}, std::byte{128},
			std::byte{128}, std::byte{255}}, .Width = 1, .Height = 1,
			.SourceChannelCount = 4};
		FImage SRGB;
		ASSERT_TRUE(Encoded.ToImage(EImageGammaSpace::SRGB, SRGB, &Error)) << Error;
		FImage Linear;
		ASSERT_TRUE(ConvertImage(SRGB.GetView(), ERawImageFormat::RGBA32F,
			EImageGammaSpace::Linear, Linear, Error)) << Error;
		float First = 0.0f;
		std::memcpy(&First, Linear.GetPixels().data(), sizeof(First));
		EXPECT_NEAR(First, 0.21586f, 0.0001f);
	}

	TEST(FImageDecoderTests, DecodesMemoryToUnscaledRgba8)
	{
		FDecodedImage Image;
		std::string Error;
		ASSERT_TRUE(DecodeImageFromMemory(std::as_bytes(std::span{TransparentPngBytes}), Image, Error)) << Error;
		EXPECT_EQ(Image.Width, 2u);
		EXPECT_EQ(Image.Height, 1u);
		EXPECT_EQ(Image.SourceChannelCount, 4u);
		ASSERT_EQ(Image.Pixels.size(), 8u);
		EXPECT_EQ(Image.Pixels[0], std::byte{255});
		EXPECT_EQ(Image.Pixels[1], std::byte{0});
		EXPECT_EQ(Image.Pixels[2], std::byte{0});
		EXPECT_EQ(Image.Pixels[3], std::byte{255});
		EXPECT_TRUE(Image.bHasTransparency);
	}

	TEST(FImageDecoderTests, DecodesFileThroughSharedPath)
	{
		const std::filesystem::path Path = WriteFixture(
			"CoreTransparent.png", std::as_bytes(std::span{TransparentPngBytes}));
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
		Image.Pixels = {std::byte{255}};
		Image.Width = 1;
		std::string Error;
		EXPECT_FALSE(DecodeImageFromMemory({}, Image, Error));
		EXPECT_TRUE(Image.Pixels.empty());
		EXPECT_EQ(Image.Width, 0u);
		EXPECT_FALSE(Error.empty());

		constexpr uint8 CorruptBytes[] = {1, 2, 3, 4, 5};
		EXPECT_FALSE(DecodeImageFromMemory(std::as_bytes(std::span{CorruptBytes}), Image, Error));
		EXPECT_TRUE(Image.Pixels.empty());
		EXPECT_FALSE(Error.empty());
	}

	TEST(FImageDecoderTests, RejectsImagesOutsideCallerLimitsBeforeDecoding)
	{
		FDecodedImage Image;
		std::string Error;
		FImageDecodeLimits Limits;
		Limits.MaximumEncodedBytes = 8;
		EXPECT_FALSE(DecodeImageFromMemory(
			std::as_bytes(std::span{TransparentPngBytes}), Image, Error, Limits));
		EXPECT_EQ(Error, "The encoded image is too large.");

		const auto TransparentBytes = std::as_bytes(std::span{TransparentPngBytes});
		Durin::FByteArray OversizedPng(TransparentBytes.begin(), TransparentBytes.end());
		// The IHDR advertises 8192 x 8192 pixels; stbi_info reads it without allocating the decoded image.
		OversizedPng[16] = std::byte{0};
		OversizedPng[17] = std::byte{0};
		OversizedPng[18] = std::byte{32};
		OversizedPng[19] = std::byte{0};
		OversizedPng[20] = std::byte{0};
		OversizedPng[21] = std::byte{0};
		OversizedPng[22] = std::byte{32};
		OversizedPng[23] = std::byte{0};
		Limits.MaximumEncodedBytes = 32ull * 1024ull * 1024ull;
		Limits.MaximumDecodedPixels = 16ull * 1024ull * 1024ull;
		EXPECT_FALSE(DecodeImageFromMemory(OversizedPng, Image, Error, Limits));
		EXPECT_EQ(Error, "The decoded image is too large.");
		EXPECT_TRUE(Image.Pixels.empty());
	}

	TEST(FImageEncoderTests, EncodesCompressedRgba8ThatRoundTripsThroughCore)
	{
		constexpr uint32 Width = 64;
		constexpr uint32 Height = 64;
		Durin::FByteArray Pixels(static_cast<size_t>(Width) * Height * 4);
		for (size_t Pixel = 0; Pixel < Pixels.size() / 4; ++Pixel)
		{
			Pixels[Pixel * 4] = std::byte{24};
			Pixels[Pixel * 4 + 1] = std::byte{96};
			Pixels[Pixel * 4 + 2] = std::byte{192};
			Pixels[Pixel * 4 + 3] = std::byte{255};
		}

		Durin::FByteArray Encoded;
		ASSERT_TRUE(EncodeRgba8Png(Pixels, Width, Height, Encoded));
		EXPECT_LT(Encoded.size(), Pixels.size() / 4);

		FDecodedImage Decoded;
		std::string Error;
		ASSERT_TRUE(DecodeImageFromMemory(Encoded, Decoded, Error)) << Error;
		EXPECT_EQ(Decoded.Width, Width);
		EXPECT_EQ(Decoded.Height, Height);
		EXPECT_EQ(Decoded.Pixels, Pixels);
	}

	TEST(FImageEncoderTests, RejectsInvalidRgba8AndClearsOutput)
	{
		Durin::FByteArray Encoded = {std::byte{1}};
		EXPECT_FALSE(EncodeRgba8Png({}, 0, 1, Encoded));
		EXPECT_TRUE(Encoded.empty());

		constexpr std::array<std::byte, 3> ShortPixels = {
			std::byte{1}, std::byte{2}, std::byte{3}};
		Encoded = {std::byte{1}};
		EXPECT_FALSE(EncodeRgba8Png(ShortPixels, 1, 1, Encoded));
		EXPECT_TRUE(Encoded.empty());
	}

	TEST(FImageDecoderTests, DecodesOldAndNewRadianceScanlinesToLinearFloat)
	{
		FDecodedFloatImage Image;
		std::string Error;
		const Durin::FByteArray OldFixture = MakeOldRadianceFixture();
		ASSERT_TRUE(DecodeRadianceHDRFromMemory(OldFixture, Image, Error)) << Error;
		ASSERT_EQ(Image.Pixels.size(), 6u);
		EXPECT_EQ(Image.Width, 2u);
		EXPECT_EQ(Image.Height, 1u);
		EXPECT_FLOAT_EQ(Image.Pixels[0], 4.0f);
		EXPECT_FLOAT_EQ(Image.Pixels[1], 2.0f);
		EXPECT_FLOAT_EQ(Image.Pixels[2], 1.0f);
		EXPECT_FLOAT_EQ(Image.Pixels[3], 0.5f);
		EXPECT_FLOAT_EQ(Image.Pixels[4], 1.0f);
		EXPECT_FLOAT_EQ(Image.Pixels[5], 0.25f);

		const Durin::FByteArray NewFixture = MakeNewRadianceFixture();
		ASSERT_TRUE(DecodeRadianceHDRFromMemory(NewFixture, Image, Error)) << Error;
		ASSERT_EQ(Image.Pixels.size(), 24u);
		for (size_t Pixel = 0; Pixel < 8; ++Pixel)
		{
			EXPECT_FLOAT_EQ(Image.Pixels[Pixel * 3], 1.0f);
			EXPECT_FLOAT_EQ(Image.Pixels[Pixel * 3 + 1], 2.0f);
			EXPECT_FLOAT_EQ(Image.Pixels[Pixel * 3 + 2], 4.0f);
		}
	}

	TEST(FImageDecoderTests, RejectsMalformedTruncatedAndOversizedRadianceWithoutPartialOutput)
	{
		FDecodedFloatImage Image;
		Image.Pixels = {1.0f};
		Image.Width = 1;
		std::string Error;
		constexpr uint8 Corrupt[] = {1, 2, 3, 4};
		EXPECT_FALSE(DecodeRadianceHDRFromMemory(
			std::as_bytes(std::span{Corrupt}), Image, Error));
		EXPECT_TRUE(Image.Pixels.empty());
		EXPECT_NE(Error.find("signature"), std::string::npos);

		Durin::FByteArray Truncated = MakeNewRadianceFixture();
		Truncated.pop_back();
		EXPECT_FALSE(DecodeRadianceHDRFromMemory(Truncated, Image, Error));
		EXPECT_TRUE(Image.Pixels.empty());
		EXPECT_NE(Error.find("truncated"), std::string::npos) << Error;

		FRadianceHDRDecodeLimits Limits;
		Limits.MaximumDecodedPixels = 1;
		const Durin::FByteArray OldFixture = MakeOldRadianceFixture();
		EXPECT_FALSE(DecodeRadianceHDRFromMemory(OldFixture, Image, Error, Limits));
		EXPECT_TRUE(Image.Pixels.empty());
		EXPECT_NE(Error.find("configured limit"), std::string::npos);
	}
} // namespace Durin::Image
