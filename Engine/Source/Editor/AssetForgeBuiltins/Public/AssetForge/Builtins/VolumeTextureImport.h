#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/PackageSerialization.h"
#include "Hash/XxHash.h"
#include "Texture/VolumeTexture.h"

namespace Durin::AssetForge::Builtins
{
	struct FVolumeTextureCapturedSource
	{
		std::string Filename;
		FXxHash128 ContentHash{};
		std::span<const std::byte> Bytes;
	};

	struct FVolumeTextureImportSettings
	{
		EVolumeTextureImportFormat ImportFormat = EVolumeTextureImportFormat::PngRowMajorAtlas;
		EVolumeTextureSourceChannels Channels = EVolumeTextureSourceChannels::Red;
		uint32 SliceWidth = 128;
		uint32 SliceHeight = 128;
		uint32 Depth = 128;
		uint32 TilesX = 12;
		uint32 TilesY = 12;

		ASSETFORGEBUILTINS_API auto IsValid(std::string* OutError = nullptr) const -> bool;
		auto GetOutputFormat() const -> EVolumeTextureFormat
		{
			return Channels == EVolumeTextureSourceChannels::RGBA
				? EVolumeTextureFormat::RGBA8_UNORM
				: EVolumeTextureFormat::R8_UNORM;
		}
	};

	// Describes source-derived import suggestions without relying on file naming.
	struct FVolumeTextureAtlasInspection
	{
		bool bSucceeded = false;
		bool bHasConfidentLayout = false;
		uint32 AtlasWidth = 0;
		uint32 AtlasHeight = 0;
		uint8 SourceChannelCount = 0;
		EVolumeTextureSourceChannels SuggestedChannels =
			EVolumeTextureSourceChannels::Red;
		std::vector<FVolumeTextureImportSettings> SuggestedLayouts;
		std::string Message;

		explicit operator bool() const { return bSucceeded; }
	};

	ASSETFORGEBUILTINS_API auto InspectVolumeTextureAtlasSource(
		std::string_view FilePath) -> FVolumeTextureAtlasInspection;
	ASSETFORGEBUILTINS_API auto TranslateVolumeTextureAtlasSource(
		const FVolumeTextureCapturedSource& Source,
		const FVolumeTextureImportSettings& Settings,
		FVolumeTextureSourceData& OutSourceData,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ReimportVolumeTexture(
		DVolumeTexture& Texture,
		std::string& OutError,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> bool;
	ASSETFORGEBUILTINS_API auto ReimportVolumeTextureFromFile(
		DVolumeTexture& Texture,
		std::string_view FilePath,
		std::string& OutError,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> bool;
}
