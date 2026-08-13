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
#include "Texture/TextureCubeAuthoring.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/TextureBuilder.h"
#include "Texture2DSourceTranslation.h"

namespace Durin::StandardAssetImport
{
	namespace
	{
		bool GRegistered = false;

		auto NormalizePanorama(Asset::FDecodedImage&& Image)
			-> AssetBuild::TextureCubeBuilder::FTexturePanoramaImage
		{
			return {.Pixels = std::move(Image.Pixels), .Width = Image.Width,
				.Height = Image.Height, .SourceChannelCount = Image.SourceChannelCount,
				.bHasTransparency = Image.bHasTransparency};
		}

		auto NormalizePanorama(Asset::FDecodedFloatImage&& Image)
			-> AssetBuild::TextureCubeBuilder::FTexturePanoramaFloatImage
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
			std::string Key = AssetBuild::MakeTextureCubeDerivedDataKey(Texture, OutError);
			if (!Key.empty())
			{
				std::unique_ptr<FTextureCubePlatformData> PlatformData;
				ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
				std::string Message;
				if (AssetBuild::LoadTextureCubeDerivedData(
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
						{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels})
						&& AssetBuild::BuildTextureCubePanorama(Texture, NormalizePanorama(std::move(Panorama)),
							Hash, Source.Panorama.SourcePath, Settings, OutError);
				}
				Asset::FDecodedImage Panorama;
				return Asset::DecodeImageFromMemory(Bytes, Panorama, OutError,
					{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels})
					&& AssetBuild::BuildTextureCubePanorama(Texture, NormalizePanorama(std::move(Panorama)),
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
			return AssetBuild::BuildTextureCubeFaces(Texture, std::move(SourceData),
				Hashes, Paths, {.bSRGB = Texture.IsSRGB()}, OutError);
		}

		auto PostLoadTerrainHeightmap(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
		{
			std::string Key = AssetBuild::MakeTerrainHeightmapDerivedDataKey(Heightmap, OutError);
			if (!Key.empty())
			{
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (AssetBuild::LoadTerrainHeightmapDerivedData(Key, Payload, OutError))
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
			Asset::FDecodedGrayscale16Image Decoded;
			if (!Asset::DecodeGrayscale16PngFromMemory(Bytes, Decoded, OutError, {
				.MaximumEncodedBytes = MaximumTerrainHeightmapEncodedBytes,
				.MaximumDecodedPixels = MaximumTerrainHeightmapSamples})) return false;
			const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
			AssetBuild::FTerrainHeightmapBuildProduct Product;
			return AssetBuild::BuildTerrainHeightmap({
				.Samples = std::move(Decoded.Samples), .Width = Decoded.Width,
				.Height = Decoded.Height, .SourceContentHashLow = Hash.HashLow,
				.SourceContentHashHigh = Hash.HashHigh}, Product, OutError)
				&& AssetBuild::PublishTerrainHeightmapProduct(Heightmap, std::move(Product), {
					.SourcePath = SourcePath, .SourceFileSize = Bytes.size(),
					.bAdvanceRevision = false, .bMarkPackageDirty = false}, OutError);
		}

		auto BuildCubeValidation(
			const FTextureCubeSourceData& SourceData,
			ETextureCubeSourceLayout Layout,
			uint32 SourceWidth,
			uint32 SourceHeight,
			bool bSRGB,
			bool bHDR,
			std::string& OutError) -> FTextureCubeImportValidation
		{
			FTextureCubePlatformData PlatformData;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
				if (!AssetBuild::TextureBuilder::BuildMipChain(
					SourceData.Faces[Index], ETextureUsage::Color, bSRGB,
					PlatformData.Faces[Index], OutError)) return {false, OutError};
			PlatformData.PixelFormat = PlatformData.Faces[0].PixelFormat;
			if (!PlatformData.IsValid()) return {false, "TextureCube platform data is inconsistent."};
			return {.bValid = true, .SourceLayout = Layout,
				.SourceWidth = SourceWidth, .SourceHeight = SourceHeight,
				.Dimension = SourceData.Faces[0].Width,
				.MipCount = static_cast<uint32>(PlatformData.Faces[0].Mips.size()),
				.PixelFormat = PlatformData.PixelFormat, .bHDR = bHDR};
		}

		auto ValidateCubeFaces(
			const std::array<std::string, TextureCubeFaceCount>& Files,
			const FTextureCubeImportSettings& Settings) -> FTextureCubeImportValidation
		{
			FTextureCubeSourceData SourceData;
			std::string Error;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				std::vector<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, Files[Index])
					|| !TranslateTexture2DSource(Bytes, SourceData.Faces[Index], Error))
					return {false, std::format("{} face decode failed: {}",
						std::array<std::string_view, TextureCubeFaceCount>{
							"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"}[Index],
						Error.empty() ? "source is unavailable" : Error)};
			}
			return BuildCubeValidation(SourceData, ETextureCubeSourceLayout::SixFaces,
				SourceData.Faces[0].Width, SourceData.Faces[0].Height,
				Settings.bSRGB, false, Error);
		}

		auto ValidateCubePanorama(
			std::string_view File,
			const FTextureCubePanoramaImportSettings& Settings) -> FTextureCubeImportValidation
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, File))
				return {false, "Panorama source is unavailable."};
			std::string Error;
			FTextureCubeSourceData SourceData;
			uint32 Width = 0;
			uint32 Height = 0;
			const std::string Extension = std::filesystem::path(File).extension().generic_string();
			const bool bHDR = Asset::IsRadianceHDRExtension(Extension);
			if (bHDR)
			{
				Asset::FDecodedFloatImage Panorama;
				if (!Asset::DecodeRadianceHDRFromMemory(Bytes, Panorama, Error,
					{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels})
					|| !AssetBuild::TextureCubeBuilder::ProjectEquirectangularTextureCube(
						NormalizePanorama(std::move(Panorama)), {Settings.FaceDimension, Settings.ExposureEV}, SourceData, Error))
					return {false, Error};
				Width = Panorama.Width;
				Height = Panorama.Height;
			}
			else
			{
				Asset::FDecodedImage Panorama;
				if (!Asset::DecodeImageFromMemory(Bytes, Panorama, Error,
					{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels})
					|| !AssetBuild::TextureCubeBuilder::ProjectEquirectangularTextureCube(
						NormalizePanorama(std::move(Panorama)), {Settings.FaceDimension, Settings.ExposureEV}, SourceData, Error))
					return {false, Error};
				Width = Panorama.Width;
				Height = Panorama.Height;
			}
			return BuildCubeValidation(SourceData,
				ETextureCubeSourceLayout::EquirectangularPanorama,
				Width, Height, true, bHDR, Error);
		}

		auto RebuildCube(DTextureCube& Texture, std::string& OutError) -> bool
		{
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
						{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels})
						&& AssetBuild::BuildTextureCubePanorama(Texture, NormalizePanorama(std::move(Panorama)), Hash,
							Source.Panorama.SourcePath, Settings, OutError);
				}
				Asset::FDecodedImage Panorama;
				return Asset::DecodeImageFromMemory(Bytes, Panorama, OutError,
					{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels})
					&& AssetBuild::BuildTextureCubePanorama(Texture, NormalizePanorama(std::move(Panorama)), Hash,
						Source.Panorama.SourcePath, Settings, OutError);
			}
			std::array<std::vector<uint8>, TextureCubeFaceCount> OwnedBytes;
			std::array<std::span<const uint8>, TextureCubeFaceCount> Bytes;
			std::array<FSourcePath, TextureCubeFaceCount> Paths;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				std::filesystem::path Path;
				Paths[Index] = Source.GetFace(static_cast<ETextureCubeFace>(Index)).SourcePath;
				if (!LoadBytes(Paths[Index], OwnedBytes[Index], Path, OutError)) return false;
				Bytes[Index] = OwnedBytes[Index];
			}
			FTextureCubeSourceData SourceData;
			std::array<FXxHash128, TextureCubeFaceCount> Hashes;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				if (!TranslateTexture2DSource(Bytes[Index], SourceData.Faces[Index], OutError))
					return false;
				Hashes[Index] = FXxHash128::HashBuffer(Bytes[Index]);
			}
			return AssetBuild::BuildTextureCubeFaces(Texture, std::move(SourceData),
				Hashes, Paths, {.bSRGB = Texture.IsSRGB()}, OutError);
		}

		auto BuildCubePanoramaEncoded(
			DTextureCube& Texture, std::span<const uint8> Bytes, std::string_view Extension,
			const FSourcePath& SourcePath,
			const FTextureCubePanoramaImportSettings& Settings,
			std::string& OutError) -> bool
		{
			const FXxHash128 Hash = FXxHash128::HashBuffer(Bytes);
			if (Asset::IsRadianceHDRExtension(Extension))
			{
				Asset::FDecodedFloatImage Panorama;
				if (!Asset::DecodeRadianceHDRFromMemory(Bytes, Panorama, OutError,
					{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels}))
				{
					OutError = std::format("TextureCube panorama decode failed: {}", OutError);
					return false;
				}
				return AssetBuild::BuildTextureCubePanorama(
					Texture, NormalizePanorama(std::move(Panorama)), Hash, SourcePath, Settings, OutError);
			}
			Asset::FDecodedImage Panorama;
			if (!Asset::DecodeImageFromMemory(Bytes, Panorama, OutError,
				{.MaximumDecodedPixels = AssetBuild::TextureCubeBuilder::MaximumPanoramaPixels}))
			{
				OutError = std::format("TextureCube panorama decode failed: {}", OutError);
				return false;
			}
			return AssetBuild::BuildTextureCubePanorama(
				Texture, NormalizePanorama(std::move(Panorama)), Hash, SourcePath, Settings, OutError);
		}

		auto BuildCubeFacesEncoded(
			DTextureCube& Texture,
			const std::array<std::span<const uint8>, TextureCubeFaceCount>& Bytes,
			const std::array<FSourcePath, TextureCubeFaceCount>& Paths,
			const FTextureCubeImportSettings& Settings,
			std::string& OutError) -> bool
		{
			FTextureCubeSourceData SourceData;
			std::array<FXxHash128, TextureCubeFaceCount> Hashes;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				if (!TranslateTexture2DSource(Bytes[Index], SourceData.Faces[Index], OutError))
					return false;
				Hashes[Index] = FXxHash128::HashBuffer(Bytes[Index]);
			}
			return AssetBuild::BuildTextureCubeFaces(
				Texture, std::move(SourceData), Hashes, Paths, Settings, OutError);
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
			StandardAssetImport::ChangeTerrainHeightmapSourceReference))
		{
			UnregisterTerrainHeightmapUncookedPostLoadHandler();
			UnregisterTextureCubeUncookedPostLoadHandler();
			return false;
		}
		if (!RegisterTextureCubeAuthoringHandlers({
			.BuildPanorama = BuildCubePanoramaEncoded,
			.BuildFaces = BuildCubeFacesEncoded,
			.Rebuild = RebuildCube,
			.ValidateFaces = ValidateCubeFaces,
			.ValidatePanorama = ValidateCubePanorama}))
		{
			UnregisterTerrainHeightmapSourceChangeHandler();
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
		UnregisterTextureCubeAuthoringHandlers();
		UnregisterTerrainHeightmapSourceChangeHandler();
		UnregisterTerrainHeightmapUncookedPostLoadHandler();
		UnregisterTextureCubeUncookedPostLoadHandler();
		GRegistered = false;
	}
}
