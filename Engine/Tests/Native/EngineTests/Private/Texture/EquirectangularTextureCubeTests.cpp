#include "ImageDecoder.h"
#include "Texture/TextureCubeBuilder.h"

#include <gtest/gtest.h>

namespace Durin::AssetBuild::TextureCubeBuilder
{
	namespace
	{
		auto FixturePath(std::string_view Name) -> std::string
		{
			return (std::filesystem::path(DURIN_TEST_DATA_DIR) / "EquirectangularPanorama" / Name).generic_string();
		}

		auto FacePixel(const FTextureCubeSourceData& Cube, ETextureCubeFace Face,
			uint32 X = 0, uint32 Y = 0) -> std::array<uint8, 4>
		{
			const FTextureSourceData& Source = Cube.Faces[static_cast<size_t>(Face)];
			const size_t Offset = (static_cast<size_t>(Y) * Source.Width + X) * 4;
			return {
				Source.Pixels[Offset],
				Source.Pixels[Offset + 1],
				Source.Pixels[Offset + 2],
				Source.Pixels[Offset + 3],
			};
		}
	} // namespace

	TEST(FEquirectangularTextureCubeTests, ProjectsLDRPrincipalAxesSeamAndPoles)
	{
		Asset::FDecodedImage Panorama;
		std::string Error;
		ASSERT_TRUE(Asset::DecodeImageFromFile(FixturePath("AnalyticalLDR.tga"), Panorama, Error)) << Error;
		ASSERT_EQ(Panorama.Width, 8u);
		ASSERT_EQ(Panorama.Height, 4u);

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeSourceData Cube;
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
		Asset::FDecodedImage Panorama;
		Panorama.Width = 2;
		Panorama.Height = 1;
		Panorama.SourceChannelCount = 4;
		Panorama.Pixels = {0, 0, 0, 255, 255, 255, 255, 255};

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeSourceData Cube;
		std::string Error;
		ASSERT_TRUE(ProjectEquirectangularTextureCube(Panorama, Settings, Cube, Error)) << Error;
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveX), (std::array<uint8, 4>{188, 188, 188, 255}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::NegativeX), (std::array<uint8, 4>{188, 188, 188, 255}));
	}

	TEST(FEquirectangularTextureCubeTests, ProjectsRadianceGoldenValuesAndExposure)
	{
		Asset::FDecodedFloatImage Panorama;
		std::string Error;
		ASSERT_TRUE(Asset::DecodeRadianceHDRFromFile(FixturePath("AnalyticalHDR.hdr"), Panorama, Error)) << Error;

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeSourceData Cube;
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

		Asset::FDecodedImage LDR;
		LDR.Width = 8;
		LDR.Height = 4;
		FTextureCubeSourceData Cube;
		EXPECT_FALSE(ProjectEquirectangularTextureCube(LDR, {}, Cube, Error));
		EXPECT_TRUE(Cube.Faces[0].Pixels.empty());
		EXPECT_NE(Error.find("storage"), std::string::npos);

		Asset::FDecodedFloatImage HDR;
		HDR.Width = 2;
		HDR.Height = 1;
		HDR.Pixels.assign(6, 1.0f);
		HDR.Pixels[0] = std::numeric_limits<float>::quiet_NaN();
		EXPECT_FALSE(ProjectEquirectangularTextureCube(HDR, {}, Cube, Error));
		EXPECT_TRUE(Cube.Faces[0].Pixels.empty());
		EXPECT_NE(Error.find("nonfinite"), std::string::npos);

		HDR.Pixels[0] = 1.0f;
		Settings = {};
		Settings.ExposureEV = 17.0f;
		EXPECT_FALSE(ProjectEquirectangularTextureCube(HDR, Settings, Cube, Error));
		EXPECT_NE(Error.find("between -16 and 16"), std::string::npos);
	}

	TEST(FEquirectangularTextureCubeTests, PropagatesProjectedTransparency)
	{
		Asset::FDecodedImage Panorama;
		Panorama.Width = 2;
		Panorama.Height = 1;
		Panorama.SourceChannelCount = 4;
		Panorama.Pixels = {0, 0, 0, 0, 255, 255, 255, 255};

		FEquirectangularTextureCubeProjectionSettings Settings;
		Settings.FaceDimension = 1;
		FTextureCubeSourceData Cube;
		std::string Error;
		ASSERT_TRUE(ProjectEquirectangularTextureCube(Panorama, Settings, Cube, Error)) << Error;
		EXPECT_TRUE(std::ranges::any_of(Cube.Faces, [](const FTextureSourceData& Face) {
			return Face.bHasTransparency;
		}));
		EXPECT_EQ(FacePixel(Cube, ETextureCubeFace::PositiveX)[3], 128u);
	}
}
