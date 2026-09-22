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

		auto WriteFixture(std::string_view Name, Durin::FByteView Bytes) -> std::filesystem::path
		{
			const std::filesystem::path Path = Durin::Testing::GetTestWorkDirectory() / Name;
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			return Path;
		}

		auto MakeOldRadianceFixture() -> Durin::FByteBuffer
		{
			constexpr std::string_view Header =
				"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
			const auto HeaderBytes = std::as_bytes(std::span{Header});
			Durin::FByteBuffer Result(HeaderBytes.begin(), HeaderBytes.end());
			Result.insert(Result.end(), {std::byte{128}, std::byte{64}, std::byte{32}, std::byte{131},
				std::byte{32}, std::byte{64}, std::byte{16}, std::byte{130}});
			return Result;
		}

		auto MakeNewRadianceFixture() -> Durin::FByteBuffer
		{
			constexpr std::string_view Header =
				"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n";
			const auto HeaderBytes = std::as_bytes(std::span{Header});
			Durin::FByteBuffer Result(HeaderBytes.begin(), HeaderBytes.end());
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
		std::string Error;
		auto ImageResult1 = FImage::TryCreate(Info, FByteBuffer(8, std::byte{7}));
		Error = ImageResult1 ? std::string{} : ImageResult1.error().ToString();
		ASSERT_TRUE(ImageResult1) << Error;
		auto Image = std::move(*ImageResult1);
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
		std::string Error;
		auto ImageResult2 = Gray.ToImage(EImageGammaSpace::Linear);
		Error = ImageResult2 ? std::string{} : ImageResult2.error().ToString();
		ASSERT_TRUE(ImageResult2) << Error;
		auto GrayImage = std::move(*ImageResult2);
		EXPECT_EQ(GrayImage.GetInfo().Format, ERawImageFormat::G16);
		ASSERT_EQ(GrayImage.GetPixels().size(), Gray.Samples.size() * sizeof(uint16));
		EXPECT_EQ(std::memcmp(GrayImage.GetPixels().data(), Gray.Samples.data(),
			GrayImage.GetPixels().size()), 0);

		FDecodedImage Encoded{.Pixels = {std::byte{128}, std::byte{128},
			std::byte{128}, std::byte{255}}, .Width = 1, .Height = 1,
			.SourceChannelCount = 4};
		FImage SRGB;
		auto ImageResult3 = Encoded.ToImage(EImageGammaSpace::SRGB);
		Error = ImageResult3 ? std::string{} : ImageResult3.error().ToString();
		ASSERT_TRUE(ImageResult3) << Error;
		SRGB = std::move(*ImageResult3);
		FImage Linear;
		auto ImageResult4 = ConvertImage(SRGB.GetView(), ERawImageFormat::RGBA32F, EImageGammaSpace::Linear);
		Error = ImageResult4 ? std::string{} : ImageResult4.error().ToString();
		ASSERT_TRUE(ImageResult4) << Error;
		Linear = std::move(*ImageResult4);
		float First = 0.0f;
		std::memcpy(&First, Linear.GetPixels().data(), sizeof(First));
		EXPECT_NEAR(First, 0.21586f, 0.0001f);
	}

	TEST(FImageTests, ReturnsErrorsWithoutPartialValues)
	{
		const FImageInfo Info{.Width = 1, .Height = 1, .Format = ERawImageFormat::RGBA8};
		const auto InvalidBytes = FImage::TryCreate(Info, FByteBuffer(3));
		ASSERT_FALSE(InvalidBytes);
		EXPECT_EQ(InvalidBytes.error().Code, EImageError::InvalidImage);
		EXPECT_FALSE(InvalidBytes.error().ToString().empty());
		const auto InvalidShared = FImage::TryCreate(Info, FSharedByteBuffer::Take(FByteBuffer(3)));
		ASSERT_FALSE(InvalidShared);
		EXPECT_EQ(InvalidShared.error().Code, EImageError::InvalidImage);
		const auto InvalidConversion = ConvertImage({}, ERawImageFormat::RGBA8, EImageGammaSpace::Linear);
		ASSERT_FALSE(InvalidConversion);
		EXPECT_EQ(InvalidConversion.error().Code, EImageError::InvalidConversion);
		const auto InvalidAnalysis = AnalyzeImageChannels({});
		ASSERT_FALSE(InvalidAnalysis);
		EXPECT_EQ(InvalidAnalysis.error().Code, EImageError::InvalidImage);
		const FDecodedFloatImage InvalidFloat{.Pixels = {1.0f}, .Width = 1, .Height = 1};
		const auto InvalidFloatImage = InvalidFloat.ToImage();
		ASSERT_FALSE(InvalidFloatImage);
		EXPECT_EQ(InvalidFloatImage.error().Code, EImageError::InvalidImage);
	}

	TEST(FImageTests, ReturnsChannelAnalysisAndKeepsSourceAfterConversionFailure)
	{
		const FDecodedFloatImage Decoded{.Pixels = {0.25f, 0.5f, 1.0f}, .Width = 1, .Height = 1};
		const auto FloatImage = Decoded.ToImage();
		ASSERT_TRUE(FloatImage);
		const auto Opaque = AnalyzeImageChannels(FloatImage->GetView());
		ASSERT_TRUE(Opaque);
		EXPECT_EQ(Opaque->MeaningfulChannelCount, 4);
		EXPECT_FALSE(Opaque->bHasTransparency);
		EXPECT_TRUE(Opaque->bAllFinite);
		const auto Failed = ConvertImage(FloatImage->GetView(), ERawImageFormat::Invalid, EImageGammaSpace::Linear);
		ASSERT_FALSE(Failed);
		EXPECT_TRUE(FloatImage->IsValid());
		const auto Transparent = FImage::TryCreate(
			{.Width = 1, .Height = 1, .Format = ERawImageFormat::RGBA8}, FByteBuffer(4));
		ASSERT_TRUE(Transparent);
		const auto Analysis = AnalyzeImageChannels(Transparent->GetView());
		ASSERT_TRUE(Analysis);
		EXPECT_TRUE(Analysis->bHasTransparency);
	}

	TEST(FImageDecoderTests, DecodesMemoryToUnscaledRgba8)
	{
		auto Decoded = DecodeImageFromMemory(std::as_bytes(std::span{TransparentPngBytes}));
		ASSERT_TRUE(Decoded) << Durin::Image::ToString(Decoded.error());
		auto Image = std::move(*Decoded);
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
		auto Decoded = DecodeImageFromFile(Path.generic_string());
		ASSERT_TRUE(Decoded) << Durin::Image::ToString(Decoded.error());
		auto Image = std::move(*Decoded);
		EXPECT_EQ(Image.Width, 2u);
		EXPECT_EQ(Image.Height, 1u);
		EXPECT_TRUE(Image.bHasTransparency);
	}

	TEST(FImageDecoderTests, RejectsEmptyAndCorruptDataWithoutPartialOutput)
	{
		auto Empty = DecodeImageFromMemory({});
		ASSERT_FALSE(Empty);
		EXPECT_EQ(Empty.error().Code, EImageDecodeError::Empty);

		constexpr uint8 CorruptBytes[] = {1, 2, 3, 4, 5};
		auto Corrupt = DecodeImageFromMemory(std::as_bytes(std::span{CorruptBytes}));
		ASSERT_FALSE(Corrupt);
		EXPECT_EQ(Corrupt.error().Code, EImageDecodeError::InvalidImage);
	}

	TEST(FImageDecoderTests, RejectsImagesOutsideCallerLimitsBeforeDecoding)
	{
		FImageDecodeLimits Limits;
		Limits.MaximumEncodedBytes = 8;
		auto EncodedLimit = DecodeImageFromMemory(std::as_bytes(std::span{TransparentPngBytes}), Limits);
		ASSERT_FALSE(EncodedLimit);
		EXPECT_EQ(EncodedLimit.error().Code, EImageDecodeError::EncodedLimit);
		EXPECT_EQ(EncodedLimit.error().Limits.MaximumEncodedBytes, 8u);
		EXPECT_EQ(EncodedLimit.error().EncodedBytes, sizeof(TransparentPngBytes));

		const auto TransparentBytes = std::as_bytes(std::span{TransparentPngBytes});
		Durin::FByteBuffer OversizedPng(TransparentBytes.begin(), TransparentBytes.end());
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
		auto PixelLimit = DecodeImageFromMemory(OversizedPng, Limits);
		ASSERT_FALSE(PixelLimit);
		EXPECT_EQ(PixelLimit.error().Code, EImageDecodeError::PixelLimit);
		EXPECT_EQ(PixelLimit.error().Width, 8192);
		EXPECT_EQ(PixelLimit.error().Height, 8192);
		EXPECT_EQ(PixelLimit.error().Limits.MaximumDecodedPixels, 16ull * 1024ull * 1024ull);
	}

	TEST(FImageDecoderTests, RetainsFileFailureIdentityAndSystemCause)
	{
		const std::array<std::byte, 2> Bytes{std::byte{1}, std::byte{2}};
		const auto Path = WriteFixture("InvalidImage.png", Bytes);

		std::string Filename = Path.generic_string();
		auto Invalid = DecodeImageFromFile(Filename);
		Filename.clear();
		ASSERT_FALSE(Invalid);
		EXPECT_EQ(Invalid.error().Code, EImageDecodeError::InvalidImage);
		EXPECT_EQ(Invalid.error().Filename, Path.generic_string());
		EXPECT_EQ(Invalid.error().EncodedBytes, Bytes.size());

		auto Missing = DecodeImageFromFile(Path.generic_string() + ".missing");
		ASSERT_FALSE(Missing);
		EXPECT_EQ(Missing.error().Code, EImageDecodeError::FileStat);
		ASSERT_TRUE(Missing.error().FileError);
		EXPECT_EQ(Missing.error().FileError->NativeError, std::errc::no_such_file_or_directory);
		EXPECT_EQ(Missing.error().Filename, Path.generic_string() + ".missing");
	}

	TEST(FImageEncoderTests, EncodesCompressedRgba8ThatRoundTripsThroughCore)
	{
		constexpr uint32 Width = 64;
		constexpr uint32 Height = 64;
		Durin::FByteBuffer Pixels(static_cast<size_t>(Width) * Height * 4);
		for (size_t Pixel = 0; Pixel < Pixels.size() / 4; ++Pixel)
		{
			Pixels[Pixel * 4] = std::byte{24};
			Pixels[Pixel * 4 + 1] = std::byte{96};
			Pixels[Pixel * 4 + 2] = std::byte{192};
			Pixels[Pixel * 4 + 3] = std::byte{255};
		}

		Durin::FByteBuffer Encoded;
		ASSERT_TRUE(EncodeRgba8Png(Pixels, Width, Height, Encoded));
		EXPECT_LT(Encoded.size(), Pixels.size() / 4);

		auto Result = DecodeImageFromMemory(Encoded);
		ASSERT_TRUE(Result) << Durin::Image::ToString(Result.error());
		auto Decoded = std::move(*Result);
		EXPECT_EQ(Decoded.Width, Width);
		EXPECT_EQ(Decoded.Height, Height);
		EXPECT_EQ(Decoded.Pixels, Pixels);
	}

	TEST(FImageEncoderTests, RejectsInvalidRgba8AndClearsOutput)
	{
		Durin::FByteBuffer Encoded = {std::byte{1}};
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
		const Durin::FByteBuffer OldFixture = MakeOldRadianceFixture();
		auto Image = DecodeRadianceHDRFromMemory(OldFixture);
		ASSERT_TRUE(Image) << ToString(Image.error());
		ASSERT_EQ(Image->Pixels.size(), 6u);
		EXPECT_EQ(Image->Width, 2u);
		EXPECT_EQ(Image->Height, 1u);
		EXPECT_FLOAT_EQ(Image->Pixels[0], 4.0f);
		EXPECT_FLOAT_EQ(Image->Pixels[1], 2.0f);
		EXPECT_FLOAT_EQ(Image->Pixels[2], 1.0f);
		EXPECT_FLOAT_EQ(Image->Pixels[3], 0.5f);
		EXPECT_FLOAT_EQ(Image->Pixels[4], 1.0f);
		EXPECT_FLOAT_EQ(Image->Pixels[5], 0.25f);

		const Durin::FByteBuffer NewFixture = MakeNewRadianceFixture();
		Image = DecodeRadianceHDRFromMemory(NewFixture);
		ASSERT_TRUE(Image) << ToString(Image.error());
		ASSERT_EQ(Image->Pixels.size(), 24u);
		for (size_t Pixel = 0; Pixel < 8; ++Pixel)
		{
			EXPECT_FLOAT_EQ(Image->Pixels[Pixel * 3], 1.0f);
			EXPECT_FLOAT_EQ(Image->Pixels[Pixel * 3 + 1], 2.0f);
			EXPECT_FLOAT_EQ(Image->Pixels[Pixel * 3 + 2], 4.0f);
		}
	}

	TEST(FImageDecoderTests, RejectsMalformedTruncatedAndOversizedRadianceWithoutPartialOutput)
	{
		constexpr uint8 Corrupt[] = {1, 2, 3, 4};
		auto Result = DecodeRadianceHDRFromMemory(std::as_bytes(std::span{Corrupt}));
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ERadianceHDRDecodeError::InvalidSignature);
		EXPECT_EQ(Result.error().EncodedBytes, sizeof(Corrupt));

		Durin::FByteBuffer Truncated = MakeNewRadianceFixture();
		Truncated.pop_back();
		Result = DecodeRadianceHDRFromMemory(Truncated);
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ERadianceHDRDecodeError::TruncatedRun);
		EXPECT_EQ(Result.error().Offset, Truncated.size());

		FRadianceHDRDecodeLimits Limits;
		Limits.MaximumDecodedPixels = 1;
		const Durin::FByteBuffer OldFixture = MakeOldRadianceFixture();
		Result = DecodeRadianceHDRFromMemory(OldFixture, Limits);
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ERadianceHDRDecodeError::PixelLimit);
		EXPECT_EQ(Result.error().Width, 2u);
		EXPECT_EQ(Result.error().Height, 1u);
		EXPECT_EQ(Result.error().Limits.MaximumDecodedPixels, 1u);
	}

	TEST(FImageDecoderTests, FileDecodeErrorsRetainNativePathAndOperation)
	{
		const auto Missing = Durin::Testing::GetTestWorkDirectory() / "Missing-image-input";
		const auto Hdr = DecodeRadianceHDRFromFile(Missing.generic_string());
		ASSERT_FALSE(Hdr);
		EXPECT_EQ(Hdr.error().Code, ERadianceHDRDecodeError::FileStat);
		ASSERT_TRUE(Hdr.error().FileError);
		EXPECT_EQ(Hdr.error().FileError->Operation, EFileOperation::QuerySize);
		EXPECT_EQ(Hdr.error().FileError->Path, Missing);
		EXPECT_TRUE(Hdr.error().FileError->NativeError);
		const auto Gray = DecodeGrayscale16PngFromFile(Missing.generic_string());
		ASSERT_FALSE(Gray);
		EXPECT_EQ(Gray.error().Code, EGrayscale16DecodeError::FileStat);
		ASSERT_TRUE(Gray.error().FileError);
		EXPECT_EQ(Gray.error().FileError->Path, Missing);
		EXPECT_TRUE(Gray.error().FileError->NativeError);
	}

	TEST(FImageDecoderTests, Grayscale16PreservesSamplesAndRejectsWrongFormatsBeforePublication)
	{
		// Independent PNG fixture: one row with 0x0000, 0x1234 and 0xffff samples.
		constexpr uint8 Bytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
			0, 0, 0, 3, 0, 0, 0, 1, 16, 0, 0, 0, 0, 110, 27, 151,
			43, 0, 0, 0, 15, 73, 68, 65, 84, 120, 156, 99, 96, 96, 16, 50,
			249, 255, 31, 0, 3, 232, 2, 69, 213, 143, 72, 72, 0, 0, 0, 0,
			73, 69, 78, 68, 174, 66, 96, 130};
		const auto View = std::as_bytes(std::span{Bytes});
		auto Result = DecodeGrayscale16PngFromMemory(View);
		ASSERT_TRUE(Result) << ToString(Result.error());
		EXPECT_EQ(Result->Width, 3u);
		EXPECT_EQ(Result->Height, 1u);
		EXPECT_EQ(Result->Samples, (std::vector<uint16>{0x0000, 0x1234, 0xffff}));
		const auto File = WriteFixture("gray16.png", View);
		const auto FromFile = DecodeGrayscale16PngFromFile(File.generic_string());
		ASSERT_TRUE(FromFile) << ToString(FromFile.error());
		EXPECT_EQ(FromFile->Samples, Result->Samples);

		Result = DecodeGrayscale16PngFromMemory(View, {.MaximumDecodedPixels = 2});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, EGrayscale16DecodeError::PixelLimit);
		EXPECT_EQ(Result.error().Width, 3u);
		EXPECT_EQ(Result.error().Limits.MaximumDecodedPixels, 2u);
		Result = DecodeGrayscale16PngFromMemory(std::as_bytes(std::span{TransparentPngBytes}));
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, EGrayscale16DecodeError::UnsupportedSampleFormat);
		Result = DecodeGrayscale16PngFromMemory(View.first(32));
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, EGrayscale16DecodeError::InvalidSignature);
	}
} // namespace Durin::Image
