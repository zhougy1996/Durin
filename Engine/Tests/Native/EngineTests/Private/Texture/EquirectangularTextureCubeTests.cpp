#include "Image/ImageDecoder.h"
#include "Texture/TextureCubeBuilder.h"

#include <gtest/gtest.h>

namespace Durin::TextureCubeBuilder
{
	namespace
	{
		auto FixturePath(std::string_view Name) -> std::string
		{
			return (std::filesystem::path(DURIN_TEST_DATA_DIR) / "EquirectangularPanorama" / Name).generic_string();
		}

		auto FacePixel(const FTextureCubeDecodedFaces& Cube, ETextureCubeFace Face,
			uint32 X = 0, uint32 Y = 0) -> std::array<uint8, 4>
		{
			const Image::FImage& Source = Cube.Faces[static_cast<size_t>(Face)];
			const size_t Offset = (static_cast<size_t>(Y) * Source.GetInfo().Width + X) * 4;
			return {
				std::to_integer<uint8>(Source.GetPixels()[Offset]),
				std::to_integer<uint8>(Source.GetPixels()[Offset + 1]),
				std::to_integer<uint8>(Source.GetPixels()[Offset + 2]),
				std::to_integer<uint8>(Source.GetPixels()[Offset + 3]),
			};
		}
	} // namespace

	TEST(FEquirectangularTextureCubeTests, ProjectsLDRPrincipalAxesSeamAndPoles)
	{
		Image::FDecodedImage Decoded;
		std::string Error;
		ASSERT_TRUE(Image::DecodeImageFromFile(FixturePath("AnalyticalLDR.tga"), Decoded, Error)) << Error;
		FTexturePanoramaImage Panorama{.Pixels = std::move(Decoded.Pixels),
			.Width = Decoded.Width, .Height = Decoded.Height,
			.SourceChannelCount = Decoded.SourceChannelCount,
			.bHasTransparency = Decoded.bHasTransparency};
		ASSERT_EQ(Panorama.Width, 8u);
		ASSERT_EQ(Panorama.Height, 4u);

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeDecodedFaces Cube;
		ASSERT_TRUE(ProjectEquirectangularTextureCube(Panorama, Settings, Cube, Error)) << Error;
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveX), (std::array<uint8, 4>{0, 255, 0, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeX), (std::array<uint8, 4>{255, 0, 0, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveY), (std::array<uint8, 4>{255, 255, 0, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeY), (std::array<uint8, 4>{0, 0, 255, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveZ), (std::array<uint8, 4>{255, 0, 255, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeZ), (std::array<uint8, 4>{0, 255, 255, 255}));
	}

	TEST(FEquirectangularTextureCubeTests, InterpolatesLDRInLinearSpaceAndWrapsTheLongitudeSeam)
	{
		FTexturePanoramaImage Panorama;
		Panorama.Width = 2;
		Panorama.Height = 1;
		Panorama.SourceChannelCount = 4;
		Panorama.Pixels = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255},
			std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeDecodedFaces Cube;
		std::string Error;
		ASSERT_TRUE(ProjectEquirectangularTextureCube(Panorama, Settings, Cube, Error)) << Error;
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveX), (std::array<uint8, 4>{188, 188, 188, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeX), (std::array<uint8, 4>{188, 188, 188, 255}));
	}

	TEST(FEquirectangularTextureCubeTests, ProjectsRadianceGoldenValuesAndExposure)
	{
		Image::FDecodedFloatImage Decoded;
		std::string Error;
		ASSERT_TRUE(Image::DecodeRadianceHDRFromFile(FixturePath("AnalyticalHDR.hdr"), Decoded, Error)) << Error;
		FTexturePanoramaFloatImage Panorama{.Pixels = std::move(Decoded.Pixels),
			.Width = Decoded.Width, .Height = Decoded.Height};

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeDecodedFaces Cube;
		ASSERT_TRUE(ProjectEquirectangularTextureCube(Panorama, Settings, Cube, Error)) << Error;
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveX), (std::array<uint8, 4>{232, 245, 252, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeX), (std::array<uint8, 4>{115, 165, 206, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveY), (std::array<uint8, 4>{245, 252, 255, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeY), (std::array<uint8, 4>{206, 232, 245, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveZ), (std::array<uint8, 4>{252, 206, 115, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeZ), (std::array<uint8, 4>{255, 252, 232, 255}));

		Panorama.Width = 2;
		Panorama.Height = 1;
		Panorama.Pixels.assign(6, 0.18f);
		Settings.ExposureEV = 2.0f;
		ASSERT_TRUE(ProjectEquirectangularTextureCube(Panorama, Settings, Cube, Error)) << Error;
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveX), (std::array<uint8, 4>{221, 221, 221, 255}));
	}

	TEST(FEquirectangularTextureCubeTests, HDRConstantRadianceSurvivesEveryOrdinaryMip)
	{
		std::array<float, 32> Pixels;
		for (size_t Index = 0; Index < 8; ++Index)
		{
			Pixels[Index * 4] = 2.0f;
			Pixels[Index * 4 + 1] = 4.0f;
			Pixels[Index * 4 + 2] = 8.0f;
			Pixels[Index * 4 + 3] = 1.0f;
		}
		Image::FImage Panorama;
		std::string Error;
		const auto Bytes = std::as_bytes(std::span(Pixels));
		ASSERT_TRUE(Image::FImage::TryCreate({.Width = 4, .Height = 2,
			.Format = Image::ERawImageFormat::RGBA32F, .GammaSpace = Image::EImageGammaSpace::Linear},
			FByteBuffer(Bytes.begin(), Bytes.end()), Panorama, &Error)) << Error;
		FTextureCubePlatformData Cube;
		ASSERT_TRUE(BuildHDRTextureCube(Panorama,
			{.FaceDimension = 8, .ExposureEV = 1.0f, .Output = ETextureCubeOutput::HDR}, Cube, Error)) << Error;
		for (const auto& Face : Cube.Faces)
		{
			ASSERT_EQ(Face.Mips.size(), 4u);
			for (const auto& Mip : Face.Mips)
				for (size_t Offset = 0; Offset < Mip.Pixels.size(); Offset += 16)
				{
					std::array<float, 4> Pixel;
					std::memcpy(Pixel.data(), Mip.Pixels.data() + Offset, 16);
					EXPECT_EQ(Pixel, (std::array<float, 4>{4, 8, 16, 1}));
				}
		}
		EXPECT_FALSE(BuildHDRTextureCube(Panorama,
			{.FaceDimension = 513, .Output = ETextureCubeOutput::HDR}, Cube, Error));
		EXPECT_FALSE(Cube.IsValid());
		EXPECT_FALSE(BuildHDRTextureCube(Panorama,
			{.FaceDimension = 8, .ExposureEV = 16, .Output = ETextureCubeOutput::HDR}, Cube, Error));
		EXPECT_FALSE(Cube.IsValid());
	}

	TEST(FEquirectangularTextureCubeTests, RejectsInvalidDimensionsStorageExposureAndAllocationLimits)
	{
		FEquirectangularTextureCubeProjectionSettings Settings;
		uint32 FaceDimension = 99;
		std::string Error;
		EXPECT_FALSE(ValidateEquirectangularTextureCubeProjection(0, 0, Settings, false, FaceDimension, Error));
		EXPECT_EQ(FaceDimension, 0u);
		EXPECT_NE(Error.find("nonzero"), std::string::npos);
		EXPECT_FALSE(ValidateEquirectangularTextureCubeProjection(8, 3, Settings, false, FaceDimension, Error));
		EXPECT_NE(Error.find("2:1"), std::string::npos);
		EXPECT_FALSE(ValidateEquirectangularTextureCubeProjection(16384, 8192, Settings, false, FaceDimension, Error));
		EXPECT_NE(Error.find("33554432"), std::string::npos);

		Settings.FaceDimension = MaximumProjectedCubeFaceDimension + 1;
		EXPECT_FALSE(ValidateEquirectangularTextureCubeProjection(8, 4, Settings, false, FaceDimension, Error));
		EXPECT_NE(Error.find("4096"), std::string::npos);

		FTexturePanoramaImage LDR;
		LDR.Width = 8;
		LDR.Height = 4;
		FTextureCubeDecodedFaces Cube;
		EXPECT_FALSE(ProjectEquirectangularTextureCube(LDR, {}, Cube, Error));
		EXPECT_FALSE(Cube.Faces[0].IsValid());
		EXPECT_NE(Error.find("storage"), std::string::npos);

		FTexturePanoramaFloatImage HDR;
		HDR.Width = 2;
		HDR.Height = 1;
		HDR.Pixels.assign(6, 1.0f);
		HDR.Pixels[0] = std::numeric_limits<float>::quiet_NaN();
		EXPECT_FALSE(ProjectEquirectangularTextureCube(HDR, {}, Cube, Error));
		EXPECT_FALSE(Cube.Faces[0].IsValid());
		EXPECT_NE(Error.find("nonfinite"), std::string::npos);

		HDR.Pixels[0] = 1.0f;
		Settings = {};
		Settings.ExposureEV = 17.0f;
		EXPECT_FALSE(ProjectEquirectangularTextureCube(HDR, Settings, Cube, Error));
		EXPECT_NE(Error.find("between -16 and 16"), std::string::npos);
	}

	TEST(FEquirectangularTextureCubeTests, DecodedFacesShareSourceStorageAndRetainMetadataAfterOwnerRelease)
	{
		FTextureCubeDecodedFaces Faces;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			ASSERT_TRUE(Image::FImage::TryCreate({.Width = 2, .Height = 2,
				.Format = Image::ERawImageFormat::RGBA8},
				FByteBuffer(16, static_cast<std::byte>(Index + 1)), Faces.Faces[Index]));
		}
		Faces.SourceChannelCounts.fill(4);
		Faces.TransparencyMask = 0x21;
		FTextureCubeDecodedFaces Decoded;
		{
			auto Source = PrepareTextureCubeSource(Faces);
			ASSERT_TRUE(Source);
			const auto Identity = Source->GetIdentity();
			Decoded = ReadTextureCubeFaces(*Source);
			ASSERT_TRUE(Decoded.IsValid());
			const auto Payload = Source->GetMipData().GetData();
			for (const auto& Face : Decoded.Faces)
				EXPECT_TRUE(Face.GetView().GetBuffer().SharesStorageWith(Payload));
			auto RoundTrip = PrepareTextureCubeSource(Decoded);
			ASSERT_TRUE(RoundTrip);
			EXPECT_EQ(RoundTrip->GetIdentity(), Identity);
		}
		EXPECT_EQ(Decoded.SourceChannelCounts, Faces.SourceChannelCounts);
		EXPECT_EQ(Decoded.TransparencyMask, 0x21);
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			EXPECT_EQ(Decoded.Faces[Index].GetPixels().size(), 16u);
			EXPECT_EQ(Decoded.Faces[Index].GetPixels().front(), static_cast<std::byte>(Index + 1));
		}
		Decoded.SourceChannelCounts[1] = 3;
		EXPECT_FALSE(Decoded.IsValid());
	}

	TEST(FEquirectangularTextureCubeTests, PropagatesProjectedTransparency)
	{
		FTexturePanoramaImage Panorama;
		Panorama.Width = 2;
		Panorama.Height = 1;
		Panorama.SourceChannelCount = 4;
		Panorama.Pixels = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
			std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeDecodedFaces Cube;
		std::string Error;
		ASSERT_TRUE(ProjectEquirectangularTextureCube(Panorama, Settings, Cube, Error)) << Error;
		EXPECT_NE(Cube.TransparencyMask, 0u);
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveX)[3], 128u);
	}
}
