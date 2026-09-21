#include <gtest/gtest.h>

#include "Math/Operations.h"
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

		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateTextureCreateDesc(Desc))) << FormatRHIError(Error.error());
	}

	TEST(FRHITextureTests, RejectsInvalidCubeDescriptions)
	{
		FRHIOperationResult Error;

		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube("TestCube")
			.SetExtent(16, 8)
			.SetFormat(EPixelFormat::RGBA8_UNORM);
		ASSERT_FALSE((Error = ValidateTextureCreateDesc(Desc)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureCreateError::NonSquareCube});

		Desc.SetExtent(16).SetArraySize(5);
		ASSERT_FALSE((Error = ValidateTextureCreateDesc(Desc)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureCreateError::InvalidCubeLayers});

		Desc.SetArraySize(TextureCubeFaceCount).SetNumSamples(4);
		ASSERT_FALSE((Error = ValidateTextureCreateDesc(Desc)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureCreateError::MultisampledDimension});
	}

	TEST(FRHITextureTests, ValidatesEveryTextureDimensionBoundary)
	{
		struct FCase
		{
			FRHITextureCreateDesc Desc;
			bool bValid;
			ERHITextureCreateError Code;
		};
		const std::array Cases{
			FCase{FRHITextureCreateDesc::Create2D("2D").SetFormat(EPixelFormat::RGBA8_UNORM), true, ERHITextureCreateError{}},
			FCase{FRHITextureCreateDesc::Create2D("2DDepth").SetDepth(2).SetFormat(EPixelFormat::RGBA8_UNORM), false, ERHITextureCreateError::Invalid2DDepth},
			FCase{FRHITextureCreateDesc::Create2DArray("Array").SetArraySize(4).SetFormat(EPixelFormat::RGBA8_UNORM), true, ERHITextureCreateError{}},
			FCase{FRHITextureCreateDesc::Create3D("3D").SetDepth(8).SetExtent(8).SetNumMips(4).SetFormat(EPixelFormat::RGBA8_UNORM), true, ERHITextureCreateError{}},
			FCase{FRHITextureCreateDesc::CreateCube("Cube").SetExtent(8).SetFormat(EPixelFormat::RGBA8_UNORM), true, ERHITextureCreateError{}},
			FCase{FRHITextureCreateDesc::CreateCubeArray("CubeArray").SetArraySize(12).SetExtent(8).SetFormat(EPixelFormat::RGBA8_UNORM), true, ERHITextureCreateError{}},
			FCase{FRHITextureCreateDesc::CreateCubeArray("BadCubeArray").SetArraySize(7).SetExtent(8).SetFormat(EPixelFormat::RGBA8_UNORM), false, ERHITextureCreateError::InvalidCubeArrayLayers},
		};
		for (const FCase& Case : Cases)
		{
			FRHIOperationResult Error;
			ASSERT_EQ((Error = ValidateTextureCreateDesc(Case.Desc)).has_value(), Case.bValid);
			if (!Case.bValid) EXPECT_EQ(Error.error().Code, FRHIError::FCode{Case.Code}) << FormatRHIError(Error.error());
		}
	}

	TEST(FRHITextureTests, RejectsInvalidMipSampleAndUsageContracts)
	{
		auto ExpectRejected = [](const FRHITextureCreateDesc& Desc, ERHITextureCreateError Expected) {
			FRHIOperationResult Error;
			ASSERT_FALSE((Error = ValidateTextureCreateDesc(Desc)));
			EXPECT_EQ(Error.error().Code, FRHIError::FCode{Expected}) << FormatRHIError(Error.error());
		};
		ExpectRejected(FRHITextureCreateDesc::Create2D("Samples").SetNumSamples(3).SetFormat(EPixelFormat::RGBA8_UNORM), ERHITextureCreateError::InvalidSampleCount);
		ExpectRejected(FRHITextureCreateDesc::Create2D("Mips").SetExtent(4).SetNumMips(4).SetFormat(EPixelFormat::RGBA8_UNORM), ERHITextureCreateError::TooManyMips);
		ExpectRejected(FRHITextureCreateDesc::Create2D("MSAAMips").SetExtent(4).SetNumMips(2).SetNumSamples(4).SetFormat(EPixelFormat::RGBA8_UNORM), ERHITextureCreateError::MultisampledMips);
		ExpectRejected(FRHITextureCreateDesc::Create2D("DepthColor").SetFlags(ETextureCreateFlags::DepthStencilTargetable | ETextureCreateFlags::RenderTargetable).SetFormat(EPixelFormat::D32), ERHITextureCreateError::ConflictingDepthUsage);
		ExpectRejected(FRHITextureCreateDesc::Create2D("StorageMSAA").SetFlags(ETextureCreateFlags::Storage).SetNumSamples(4).SetFormat(EPixelFormat::RGBA8_UNORM), ERHITextureCreateError::MultisampledUsage);
	}

	TEST(FRHITextureTests, ValidatesMipSliceRegionAndSourcePitch)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube("TestCube")
			.SetExtent(8)
			.SetNumMips(4)
			.SetFormat(EPixelFormat::RGBA8_UNORM);
		const FUpdateTextureRegion2D FullMip(0, 0, 0, 0, 4, 4);
		FRHIOperationResult Error;

		EXPECT_TRUE((Error = ValidateTexture2DUpdate(Desc, 1, 5, FullMip, 16))) << FormatRHIError(Error.error());
		ASSERT_FALSE((Error = ValidateTexture2DUpdate(Desc, 4, 0, FullMip, 16)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureUploadError::MipOutOfBounds});
		ASSERT_FALSE((Error = ValidateTexture2DUpdate(Desc, 0, 6, FullMip, 16)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureUploadError::LayerOutOfBounds});
		ASSERT_FALSE((Error = ValidateTexture2DUpdate(Desc, 1, 0, FullMip, 15)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureUploadError::InsufficientPitch});

		const FUpdateTextureRegion2D OutsideMip(1, 0, 0, 0, 4, 4);
		ASSERT_FALSE((Error = ValidateTexture2DUpdate(Desc, 1, 0, OutsideMip, 16)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureUploadError::BoxOutOfBounds});
	}

	TEST(FRHITextureTests, ValidatesBlockCompressedUploadAlignment)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("Compressed")
			.SetExtent(FIntPoint(10, 6))
			.SetNumMips(2)
			.SetFormat(EPixelFormat::BC1_UNORM);
		FRHIOperationResult Error;

		const FUpdateTextureRegion2D FullMip(0, 0, 0, 0, 10, 6);
		EXPECT_TRUE((Error = ValidateTexture2DUpdate(Desc, 0, 0, FullMip, 24))) << FormatRHIError(Error.error());

		const FUpdateTextureRegion2D TailMip(0, 0, 0, 0, 5, 3);
		EXPECT_TRUE((Error = ValidateTexture2DUpdate(Desc, 1, 0, TailMip, 16))) << FormatRHIError(Error.error());

		const FUpdateTextureRegion2D MisalignedOffset(2, 0, 0, 0, 4, 4);
		ASSERT_FALSE((Error = ValidateTexture2DUpdate(Desc, 0, 0, MisalignedOffset, 8)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureUploadError::OffsetAlignment});

		const FUpdateTextureRegion2D PartialBlock(0, 0, 0, 0, 5, 4);
		ASSERT_FALSE((Error = ValidateTexture2DUpdate(Desc, 0, 0, PartialBlock, 16)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureUploadError::ExtentAlignment});
	}

	TEST(FRHITextureTests, ValidatesVolumeCreationViewsAndOddRegionPitches)
	{
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D("Volume")
			.SetExtent(7, 5).SetDepth(3).SetNumMips(3)
			.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::Storage
				| ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::DestinationCopy);
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = ValidateTextureCreateDesc(Desc))) << FormatRHIError(Error.error());
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>(Desc);
		EXPECT_EQ(Texture->GetSizeZ(), 3u);
		const FRHITextureViewDesc Sampled = MakeDefaultTextureViewDesc(
			*Texture, ERHITextureViewUsage::Sampled);
		EXPECT_EQ(Sampled.Dimension, ERHITextureViewDimension::Texture3D);
		EXPECT_TRUE((Error = ValidateTextureViewDesc(Texture, Sampled))) << FormatRHIError(Error.error());
		FRHITextureViewDesc Storage = MakeDefaultTextureViewDesc(
			*Texture, ERHITextureViewUsage::Storage);
		EXPECT_TRUE((Error = ValidateTextureViewDesc(Texture, Storage))) << FormatRHIError(Error.error());

		const FUpdateTextureRegion3D Region(1, 1, 0, 2, 1, 1, 3, 3, 2);
		EXPECT_TRUE((Error = ValidateTexture3DUpdate(Desc, 0, Region, 24, 96))) << FormatRHIError(Error.error());
		ASSERT_FALSE((Error = ValidateTexture3DUpdate(Desc, 0, Region, 19, 96)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHIVolumeUploadError::InsufficientRowPitch});
		ASSERT_FALSE((Error = ValidateTexture3DUpdate(Desc, 0, Region, 24, 80)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHIVolumeUploadError::InsufficientDepthPitch});

		Desc.AddFlags(ETextureCreateFlags::RenderTargetable);
		ASSERT_FALSE((Error = ValidateTextureCreateDesc(Desc)));
		EXPECT_EQ(Error.error().Code, FRHIError::FCode{ERHITextureCreateError::UnsupportedVolumeUsage});
	}

	TEST(FRHITextureTests, ValidatesVolumeCopyDepthAndFootprint)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D("VolumeCopy")
			.SetExtent(5, 3).SetDepth(3).SetNumMips(3)
			.SetFormat(EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::SourceCopy | ETextureCreateFlags::DestinationCopy);
		TRefCountPtr<FRHITexture> Texture = MakeRefCount<FRHITexture>(Desc);
		const FRHIBufferTextureCopyRegion Region{
			.BufferRowLength = 7, .BufferImageHeight = 4,
			.TextureMip = 0, .TextureFirstArrayLayer = 0,
			.TextureNumArrayLayers = 1, .TextureOffset = {1, 0, 1},
			.TextureExtent = {3, 3, 2}};
		uint64 Footprint = 0;
		FRHIOperationResult Error;
		EXPECT_TRUE((Error = GetBufferTextureCopyFootprint(*Texture, Region, Footprint))) << FormatRHIError(Error.error());
		EXPECT_EQ(Footprint, 56u);
		TRefCountPtr<FRHIBuffer> Buffer = MakeRefCount<FRHIBuffer>(
			FRHIBufferCreateDesc::Create("VolumeBuffer", 56, 1,
				EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
		EXPECT_TRUE((Error = ValidateBufferToTextureCopies(Buffer, Texture,
			std::span<const FRHIBufferTextureCopyRegion>(&Region, 1)))) << FormatRHIError(Error.error());
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

	TEST(FRHITextureTests, CubePixelDirectionsInvertTheDocumentedFaceConvention)
	{
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			const auto Face = static_cast<ETextureCubeFace>(FaceIndex);
			for (uint32 Y : {0u, 1u, 2u})
			{
				for (uint32 X : {0u, 1u, 2u})
				{
					FVector3 Direction{};
					ASSERT_TRUE(ResolveTextureCubeFacePixelDirection(Face, X, Y, 3, Direction));
					EXPECT_NEAR(Math::Length(Direction), 1.0, 1.e-12);

					ETextureCubeFace ResolvedFace = ETextureCubeFace::PositiveX;
					FVector2f Uv{};
					ASSERT_TRUE(ResolveTextureCubeFaceUv(Direction, ResolvedFace, Uv));
					EXPECT_EQ(ResolvedFace, Face);
					EXPECT_NEAR(Uv.x, (static_cast<float>(X) + 0.5f) / 3.0f, 1.e-6f);
					EXPECT_NEAR(Uv.y, (static_cast<float>(Y) + 0.5f) / 3.0f, 1.e-6f);
				}
			}
		}

		FVector3 Direction{};
		EXPECT_FALSE(ResolveTextureCubeFacePixelDirection(ETextureCubeFace::PositiveX, 0, 0, 0, Direction));
		EXPECT_FALSE(ResolveTextureCubeFacePixelDirection(ETextureCubeFace::PositiveX, 3, 0, 3, Direction));
		EXPECT_FALSE(ResolveTextureCubeFacePixelDirection(static_cast<ETextureCubeFace>(TextureCubeFaceCount), 0, 0, 1, Direction));
	}

	TEST(FRHITextureTests, CubePixelDirectionsMatchEveryDocumentedFaceEdge)
	{
		struct FFaceEdges
		{
			ETextureCubeFace Face;
			std::array<FVector3, 4> TopRightBottomLeft;
		};
		constexpr double Edge = 2.0 / 3.0;
		const std::array Cases{
			FFaceEdges{ETextureCubeFace::PositiveX, {
				FVector3(1, Edge, 0), FVector3(1, 0, -Edge), FVector3(1, -Edge, 0), FVector3(1, 0, Edge)}},
			FFaceEdges{ETextureCubeFace::NegativeX, {
				FVector3(-1, Edge, 0), FVector3(-1, 0, Edge), FVector3(-1, -Edge, 0), FVector3(-1, 0, -Edge)}},
			FFaceEdges{ETextureCubeFace::PositiveY, {
				FVector3(0, 1, -Edge), FVector3(Edge, 1, 0), FVector3(0, 1, Edge), FVector3(-Edge, 1, 0)}},
			FFaceEdges{ETextureCubeFace::NegativeY, {
				FVector3(0, -1, Edge), FVector3(Edge, -1, 0), FVector3(0, -1, -Edge), FVector3(-Edge, -1, 0)}},
			FFaceEdges{ETextureCubeFace::PositiveZ, {
				FVector3(0, Edge, 1), FVector3(Edge, 0, 1), FVector3(0, -Edge, 1), FVector3(-Edge, 0, 1)}},
			FFaceEdges{ETextureCubeFace::NegativeZ, {
				FVector3(0, Edge, -1), FVector3(-Edge, 0, -1), FVector3(0, -Edge, -1), FVector3(Edge, 0, -1)}},
		};
		constexpr std::array<std::pair<uint32, uint32>, 4> EdgePixels = {
			std::pair{1u, 0u}, std::pair{2u, 1u}, std::pair{1u, 2u}, std::pair{0u, 1u}};

		for (const FFaceEdges& Case : Cases)
		{
			for (size_t EdgeIndex = 0; EdgeIndex < EdgePixels.size(); ++EdgeIndex)
			{
				FVector3 Direction{};
				ASSERT_TRUE(ResolveTextureCubeFacePixelDirection(
					Case.Face, EdgePixels[EdgeIndex].first, EdgePixels[EdgeIndex].second, 3, Direction));
				const FVector3 Expected = Math::Normalize(Case.TopRightBottomLeft[EdgeIndex]);
				EXPECT_NEAR(Direction.x, Expected.x, 1.e-12);
				EXPECT_NEAR(Direction.y, Expected.y, 1.e-12);
				EXPECT_NEAR(Direction.z, Expected.z, 1.e-12);
			}
		}
	}
}
