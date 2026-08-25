#pragma once

#include "AssetTools.h"
#include "Asset/AssetCompilingManager.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/Transaction.h"
#include "Editor/PropertyView.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureBuilder.h"
#include "Texture/Texture2DAuthoringService.h"
#include "Texture/TextureBuildService.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "TexturePlatformDataTestFixtures.h"

#include <bc7decomp.h>
#include <gtest/gtest.h>
#include <rgbcx.h>
#include <unordered_set>

inline auto InitializeTextureImportMount() -> void
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureImports";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPointForTests(
			"/TextureImportTests/", (Root / "Content").generic_string() + "/");
	}
}

inline auto EnsureTextureCompilingManager() -> bool
{
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests()
		&& !Durin::InitializeAssetCompilingManager()) return false;
	return Durin::Asset::Build::InitializeTextureBuildService(
		GetEngineTestModuleCallbackGate());
}

inline auto RestartTextureCompilingManager(
	const Durin::Asset::Build::FTexture2DBuildCoordinatorConfig& Config = {}) -> bool
{
	Durin::Asset::Build::ShutdownTextureBuildService();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests()
		&& !Durin::InitializeAssetCompilingManager()) return false;
	return Durin::Asset::Build::InitializeTextureBuildService(
		GetEngineTestModuleCallbackGate(), Config);
}

namespace
{
	constexpr uint8 TransparentPngBytes[] = {
		137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
		0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
		0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

	auto WriteTextureFixture(const std::filesystem::path& Path) -> void
	{
		InitializeTextureImportMount();
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(TransparentPngBytes), sizeof(TransparentPngBytes));
	}

	auto WriteNpotTextureFixture(const std::filesystem::path& Path) -> void
	{
		InitializeTextureImportMount();
		constexpr uint16 Width = 5;
		constexpr uint16 Height = 3;
		std::array<uint8, 18> Header{};
		Header[2] = 2;
		Header[12] = static_cast<uint8>(Width);
		Header[14] = static_cast<uint8>(Height);
		Header[16] = 32;
		Header[17] = 0x28;
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Header.data()), Header.size());
		for (uint16 Y = 0; Y < Height; ++Y)
		{
			for (uint16 X = 0; X < Width; ++X)
			{
				const uint8 Value = X == Width - 1 ? 255 : 0;
				const std::array<uint8, 4> Pixel = {Value, Value, Value, 255};
				Stream.write(reinterpret_cast<const char*>(Pixel.data()), Pixel.size());
			}
		}
	}

	auto DecodeFirstCompressedPixel(Durin::EPixelFormat Format, const std::vector<std::byte>& Block)
		-> std::array<uint8, 4>
	{
		std::array<uint8, 64> Pixels{};
		switch (Format)
		{
		case Durin::EPixelFormat::BC1_UNORM:
		case Durin::EPixelFormat::BC1_UNORM_SRGB:
			EXPECT_FALSE(rgbcx::unpack_bc1(Block.data(), Pixels.data()));
			break;
		case Durin::EPixelFormat::BC3_UNORM:
		case Durin::EPixelFormat::BC3_UNORM_SRGB:
			EXPECT_TRUE(rgbcx::unpack_bc3(Block.data(), Pixels.data()));
			break;
		case Durin::EPixelFormat::BC5_UNORM:
			rgbcx::unpack_bc5(Block.data(), Pixels.data());
			break;
		case Durin::EPixelFormat::BC7_UNORM:
		case Durin::EPixelFormat::BC7_UNORM_SRGB:
			static_assert(sizeof(bc7decomp::color_rgba) == 4);
			EXPECT_TRUE(bc7decomp::unpack_bc7(Block.data(), reinterpret_cast<bc7decomp::color_rgba*>(Pixels.data())));
			break;
		default:
			ADD_FAILURE() << "Unsupported compressed test format";
			break;
		}
		return {Pixels[0], Pixels[1], Pixels[2], Pixels[3]};
	}

	auto ExpectPixelNear(const std::array<uint8, 4>& Actual,
		const std::array<uint8, 4>& Expected, int Tolerance = 24) -> void
	{
		for (size_t Channel = 0; Channel < Expected.size(); ++Channel)
			EXPECT_NEAR(Actual[Channel], Expected[Channel], Tolerance) << "channel " << Channel;
	}

	auto DecodeBC3Mip(const Durin::FTexture2DMipData& Mip) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result(static_cast<size_t>(Mip.Width) * Mip.Height * 4);
		const uint32 BlocksWide = (Mip.Width + 3) / 4;
		const uint32 BlocksHigh = (Mip.Height + 3) / 4;
		for (uint32 BlockY = 0; BlockY < BlocksHigh; ++BlockY)
		{
			for (uint32 BlockX = 0; BlockX < BlocksWide; ++BlockX)
			{
				std::array<uint8, 64> BlockPixels{};
				const uint8* Block = reinterpret_cast<const uint8*>(Mip.Pixels.data())
					+ static_cast<size_t>(BlockY) * Mip.RowPitch + BlockX * 16;
				EXPECT_TRUE(rgbcx::unpack_bc3(Block, BlockPixels.data()));
				for (uint32 Y = 0; Y < 4 && BlockY * 4 + Y < Mip.Height; ++Y)
				{
					for (uint32 X = 0; X < 4 && BlockX * 4 + X < Mip.Width; ++X)
					{
						const size_t SourceOffset = (Y * 4 + X) * 4;
						const size_t DestOffset = (static_cast<size_t>(BlockY * 4 + Y) * Mip.Width
							+ BlockX * 4 + X) * 4;
						std::memcpy(Result.data() + DestOffset, BlockPixels.data() + SourceOffset, 4);
					}
				}
			}
		}
		return Result;
	}

	auto CalculateDecodedCoverage(const std::vector<std::byte>& Pixels, uint8 Threshold) -> double
	{
		size_t CoveredPixelCount = 0;
		for (size_t Offset = 3; Offset < Pixels.size(); Offset += 4)
			if (std::to_integer<uint8>(Pixels[Offset]) >= Threshold) ++CoveredPixelCount;
		return static_cast<double>(CoveredPixelCount) / (Pixels.size() / 4);
	}

	auto GetTextureCachePath(const Durin::DTexture2D& Texture) -> std::filesystem::path
	{
		const std::string& Key = Texture.GetDerivedDataKey();
		EXPECT_GE(Key.size(), 2u);
		return std::filesystem::path(Durin::FPaths::DerivedDataCacheDir())
			/ "Textures" / "Objects" / Key.substr(0, 2) / (Key + ".bin");
	}

}
