#include <gtest/gtest.h>

#include "Math/Vector.h"
#include "RHIResources.h"

namespace Durin
{
	namespace
	{
		struct FCubeDirectionCase
		{
			FVector3 Direction;
			ETextureCubeFace Face;
			FVector2f Uv;
		};
	}

	TEST(FRHITextureTests, CubeDescriptionEstablishesSixLayerContract)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube("TestCube")
			.SetExtent(16)
			.SetNumMips(5)
			.SetFormat(EPixelFormat::SRGBA8_UNORM);

		EXPECT_EQ(Desc.Dimension, ETextureDimension::TextureCube);
		EXPECT_EQ(Desc.ArraySize, TextureCubeFaceCount);
		EXPECT_EQ(Desc.Extent, FIntPoint(16, 16));

		std::string Error;
		EXPECT_TRUE(ValidateTextureCreateDesc(Desc, Error)) << Error;
	}

	TEST(FRHITextureTests, RejectsInvalidCubeDescriptions)
	{
		std::string Error;

		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube("TestCube")
			.SetExtent(16, 8)
			.SetFormat(EPixelFormat::RGBA8_UNORM);
		EXPECT_FALSE(ValidateTextureCreateDesc(Desc, Error));
		EXPECT_NE(Error.find("equal"), std::string::npos);

		Desc.SetExtent(16).SetArraySize(5);
		EXPECT_FALSE(ValidateTextureCreateDesc(Desc, Error));
		EXPECT_NE(Error.find("six"), std::string::npos);

		Desc.SetArraySize(TextureCubeFaceCount).SetNumSamples(4);
		EXPECT_FALSE(ValidateTextureCreateDesc(Desc, Error));
		EXPECT_NE(Error.find("single-sampled"), std::string::npos);
	}

	TEST(FRHITextureTests, ValidatesMipSliceRegionAndSourcePitch)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube("TestCube")
			.SetExtent(8)
			.SetNumMips(4)
			.SetFormat(EPixelFormat::RGBA8_UNORM);
		const FUpdateTextureRegion2D FullMip(0, 0, 0, 0, 4, 4);
		std::string Error;

		EXPECT_TRUE(ValidateTexture2DUpdate(Desc, 1, 5, FullMip, 16, Error)) << Error;
		EXPECT_FALSE(ValidateTexture2DUpdate(Desc, 4, 0, FullMip, 16, Error));
		EXPECT_NE(Error.find("mip"), std::string::npos);
		EXPECT_FALSE(ValidateTexture2DUpdate(Desc, 0, 6, FullMip, 16, Error));
		EXPECT_NE(Error.find("slice"), std::string::npos);
		EXPECT_FALSE(ValidateTexture2DUpdate(Desc, 1, 0, FullMip, 15, Error));
		EXPECT_NE(Error.find("pitch"), std::string::npos);

		const FUpdateTextureRegion2D OutsideMip(1, 0, 0, 0, 4, 4);
		EXPECT_FALSE(ValidateTexture2DUpdate(Desc, 1, 0, OutsideMip, 16, Error));
		EXPECT_NE(Error.find("exceeds"), std::string::npos);
	}

	TEST(FRHITextureTests, ValidatesBlockCompressedUploadAlignment)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("Compressed")
			.SetExtent(FIntPoint(10, 6))
			.SetNumMips(2)
			.SetFormat(EPixelFormat::BC1_UNORM);
		std::string Error;

		const FUpdateTextureRegion2D FullMip(0, 0, 0, 0, 10, 6);
		EXPECT_TRUE(ValidateTexture2DUpdate(Desc, 0, 0, FullMip, 24, Error)) << Error;

		const FUpdateTextureRegion2D TailMip(0, 0, 0, 0, 5, 3);
		EXPECT_TRUE(ValidateTexture2DUpdate(Desc, 1, 0, TailMip, 16, Error)) << Error;

		const FUpdateTextureRegion2D MisalignedOffset(2, 0, 0, 0, 4, 4);
		EXPECT_FALSE(ValidateTexture2DUpdate(Desc, 0, 0, MisalignedOffset, 8, Error));
		EXPECT_NE(Error.find("block-aligned"), std::string::npos);

		const FUpdateTextureRegion2D PartialBlock(0, 0, 0, 0, 5, 4);
		EXPECT_FALSE(ValidateTexture2DUpdate(Desc, 0, 0, PartialBlock, 16, Error));
		EXPECT_NE(Error.find("mip edge"), std::string::npos);
	}

	TEST(FRHITextureTests, ResolvesDocumentedPrincipalAxesAndEdgeDirections)
	{
		const std::array Cases{
			FCubeDirectionCase{FVector3(1.0, 0.0, 0.0), ETextureCubeFace::PositiveX, FVector2f(0.5f, 0.5f)},
			FCubeDirectionCase{FVector3(-1.0, 0.0, 0.0), ETextureCubeFace::NegativeX, FVector2f(0.5f, 0.5f)},
			FCubeDirectionCase{FVector3(0.0, 1.0, 0.0), ETextureCubeFace::PositiveY, FVector2f(0.5f, 0.5f)},
			FCubeDirectionCase{FVector3(0.0, -1.0, 0.0), ETextureCubeFace::NegativeY, FVector2f(0.5f, 0.5f)},
			FCubeDirectionCase{FVector3(0.0, 0.0, 1.0), ETextureCubeFace::PositiveZ, FVector2f(0.5f, 0.5f)},
			FCubeDirectionCase{FVector3(0.0, 0.0, -1.0), ETextureCubeFace::NegativeZ, FVector2f(0.5f, 0.5f)},
			FCubeDirectionCase{FVector3(1.0, 0.0, 1.0), ETextureCubeFace::PositiveX, FVector2f(0.0f, 0.5f)},
			FCubeDirectionCase{FVector3(0.0, 1.0, -1.0), ETextureCubeFace::PositiveY, FVector2f(0.5f, 0.0f)}
		};

		for (const FCubeDirectionCase& Case : Cases)
		{
			ETextureCubeFace Face = ETextureCubeFace::PositiveX;
			FVector2f Uv{};
			ASSERT_TRUE(ResolveTextureCubeFaceUv(Case.Direction, Face, Uv));
			EXPECT_EQ(Face, Case.Face);
			EXPECT_NEAR(Uv.x, Case.Uv.x, 1.e-6f);
			EXPECT_NEAR(Uv.y, Case.Uv.y, 1.e-6f);
		}

		ETextureCubeFace Face = ETextureCubeFace::PositiveX;
		FVector2f Uv{};
		EXPECT_FALSE(ResolveTextureCubeFaceUv(FVector3(0.0), Face, Uv));
	}
}
