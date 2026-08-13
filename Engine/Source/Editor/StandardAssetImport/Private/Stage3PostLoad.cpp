#include "Stage3PostLoad.h"

#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapPostLoad.h"
#include "TerrainHeightmapSourceTranslation.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubeBuilder.h"
#include "TextureCubeBuildAdapter.h"
#include "TextureCubeSourceTranslation.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/TextureBuilder.h"
#include "Texture2DSourceTranslation.h"

namespace Durin::Asset::Import
{
	namespace
	{
		bool GRegistered = false;

		auto NormalizePanorama(Asset::FDecodedImage&& Image)
			-> Asset::Build::TextureCubeBuilder::FTexturePanoramaImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height, .SourceChannelCount = Image.SourceChannelCount,
				.bHasTransparency = Image.bHasTransparency};
		}

		auto NormalizePanorama(Asset::FDecodedFloatImage&& Image)
			-> Asset::Build::TextureCubeBuilder::FTexturePanoramaFloatImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height};
		}

		auto LoadBytes(const FSourcePath& Source, std::vector<uint8>& OutBytes,
			std::filesystem::path& OutPath, std::string& OutError) -> bool
		{
			const PathUtilities::FSourcePathResult Resolved = PathUtilities::ResolveSourcePath(
				Source.Path, PathUtilities::EPathExistence::RequireFile);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPath = Resolved.PhysicalPath;
			if (FFileHelper::LoadFileToArray(OutBytes, OutPath.generic_string())) return true;
			OutError = std::format("Failed to read mounted source '{}'.", Source.Path);
			return false;
		}

		auto PostLoadTextureCube(DTextureCube& Texture, std::string& OutError) -> bool
		{
			std::string Key = Asset::Build::MakeTextureCubeDerivedDataKey(Texture, OutError);
			if (!Key.empty())
			{
				std::unique_ptr<FTextureCubePlatformData> PlatformData;
				ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
				std::string Message;
				if (Asset::Build::LoadTextureCubeDerivedData(
					Key, PlatformData, Status, Message))
					return Texture.PublishDerivedDataLoad(
						std::move(PlatformData), std::move(Key), OutError);
			}

			const FTextureCubeSourceImportData& Source = Texture.GetSourceImportData();
			if (Source.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			{
				std::vector<uint8> Bytes;
				std::filesystem::path Path;
				if (!LoadBytes(Source.Panorama.SourcePath, Bytes, Path, OutError)) return false;
				const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
				const FTextureCubePanoramaImportSettings Settings{
					Texture.GetPanoramaFaceDimension(), Texture.GetPanoramaExposureEV()};
				if (Asset::IsRadianceHDRExtension(Path.extension().generic_string()))
				{
					Asset::FDecodedFloatImage Panorama;
					return Asset::DecodeRadianceHDRFromMemory(Bytes, Panorama, OutError,
						{.MaximumDecodedPixels = Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels})
						&& BuildAndPublishTextureCubePanorama(Texture, NormalizePanorama(std::move(Panorama)),
							Hash, Source.Panorama.SourcePath, Settings, OutError);
				}
				Asset::FDecodedImage Panorama;
				return Asset::DecodeImageFromMemory(Bytes, Panorama, OutError,
					{.MaximumDecodedPixels = Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels})
					&& BuildAndPublishTextureCubePanorama(Texture, NormalizePanorama(std::move(Panorama)),
						Hash, Source.Panorama.SourcePath, Settings, OutError);
			}

			FTextureCubeSourceData SourceData;
			std::array<FXxHash128, TextureCubeFaceCount> Hashes;
			std::array<FSourcePath, TextureCubeFaceCount> Paths;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceFile& Face = Source.GetFace(static_cast<ETextureCubeFace>(Index));
				std::vector<uint8> Bytes;
				std::filesystem::path Path;
				if (!LoadBytes(Face.SourcePath, Bytes, Path, OutError)
					|| !TranslateTexture2DSource(Bytes, SourceData.Faces[Index], OutError))
					return false;
				Hashes[Index] = FXxHash128::HashBuffer(Bytes);
				Paths[Index] = Face.SourcePath;
			}
			return BuildAndPublishTextureCubeFaces(Texture, std::move(SourceData),
				Hashes, Paths, {.bSRGB = Texture.IsSRGB()}, OutError);
		}

		auto PostLoadTerrainHeightmap(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
		{
			std::string Key = Asset::Build::MakeTerrainHeightmapDerivedDataKey(Heightmap, OutError);
			if (!Key.empty())
			{
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (Asset::Build::LoadTerrainHeightmapDerivedData(Key, Payload, OutError))
				{
					const auto& Source = Heightmap.GetSourceImportData();
					Heightmap.PublishAuthoringCandidate(Source, 0, 0, std::move(Payload),
						std::move(Key), "Loaded terrain heightmap payload from DDC.",
						false, false, true);
					return true;
				}
			}
			std::vector<uint8> Bytes;
			std::filesystem::path Path;
			const FSourcePath SourcePath = Heightmap.GetSourceImportData().SourcePath;
			if (!LoadBytes(SourcePath, Bytes, Path, OutError)) return false;
			FTerrainHeightmapDecodedSource Decoded;
			if (!DecodeTerrainHeightmapSource(
				Path.extension().generic_string(), Bytes, Decoded, OutError)) return false;
			const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
			Asset::Build::FTerrainHeightmapBuildProduct Product;
			return Asset::Build::BuildTerrainHeightmap({
				.Samples = std::move(Decoded.Samples), .Width = Decoded.Width,
				.Height = Decoded.Height, .SourceContentHashLow = Hash.HashLow,
				.SourceContentHashHigh = Hash.HashHigh,
				.DecoderId = Decoded.DecoderId,
				.DecoderVersion = Decoded.DecoderVersion,
				.SourceFormat = Decoded.SourceFormat,
				.SourceProfileVersion = Decoded.SourceProfileVersion}, Product, OutError)
				&& Asset::Build::PublishTerrainHeightmapProduct(Heightmap, std::move(Product), {
					.SourcePath = SourcePath,
					.DecoderId = Decoded.DecoderId,
					.DecoderVersion = Decoded.DecoderVersion,
					.SourceFormat = Decoded.SourceFormat,
					.SourceProfileVersion = Decoded.SourceProfileVersion,
					.SourceFileSize = Bytes.size(),
					.bAdvanceRevision = false, .bMarkPackageDirty = false}, OutError);
		}

	}

	auto RegisterStage3PostLoadPolicies() -> bool
	{
		if (GRegistered) return true;
		if (!RegisterTextureCubeUncookedPostLoadHandler(PostLoadTextureCube)) return false;
		if (!RegisterTerrainHeightmapUncookedPostLoadHandler(PostLoadTerrainHeightmap))
		{
			UnregisterTextureCubeUncookedPostLoadHandler();
			return false;
		}
		if (!RegisterTerrainHeightmapSourceChangeHandler(
			ChangeTerrainHeightmapSourceReference))
		{
			UnregisterTerrainHeightmapUncookedPostLoadHandler();
			UnregisterTextureCubeUncookedPostLoadHandler();
			return false;
		}
		GRegistered = true;
		return true;
	}

	auto UnregisterStage3PostLoadPolicies() -> void
	{
		if (!GRegistered) return;
		UnregisterTerrainHeightmapSourceChangeHandler();
		UnregisterTerrainHeightmapUncookedPostLoadHandler();
		UnregisterTextureCubeUncookedPostLoadHandler();
		GRegistered = false;
	}
}
