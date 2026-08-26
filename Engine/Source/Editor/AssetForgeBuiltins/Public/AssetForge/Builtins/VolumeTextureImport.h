#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Hash/XxHash.h"
#include "Texture/VolumeTexture.h"
#include "AssetForge/ImportRequest.h"
#include "AssetForge/Operations/ImportExecution.h"

namespace Durin::AssetForge::Builtins
{
	inline constexpr std::string_view VolumeTextureSourceProviderId = "DurinImage";
	inline constexpr uint32 VolumeTextureSourceProviderVersion = 1;

	struct FVolumeTextureCapturedSource
	{
		FSourcePath SourcePath;
		FXxHash128 ContentHash{};
		std::span<const std::byte> Bytes;
	};

	struct FVolumeTextureImportSettings
	{
		// Empty stores the PNG beneath the asset mount.
		std::string SourceDestination;
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

	struct FVolumeTextureImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DVolumeTexture* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
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
	ASSETFORGEBUILTINS_API auto BuildVolumeTextureCandidate(
		DVolumeTexture& Texture,
		const FVolumeTextureCapturedSource& Source,
		const FVolumeTextureImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ImportVolumeTextureAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FVolumeTextureImportSettings& Settings = {},
		bool bAllowEngineContentWrite = false)
		-> FVolumeTextureImportResult;
	ASSETFORGEBUILTINS_API auto RepairVolumeTextureSource(
		DVolumeTexture& Texture,
		std::string_view SourcePath,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto MakeVolumeTextureImportRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FVolumeTextureImportSettings& Settings,
		AssetForge::EImportMode Mode,
		AssetForge::FImportOperationOwner Owner,
		std::optional<AssetForge::FImportProvenance> ExistingProvenance,
		AssetForge::FImportRequest& OutRequest,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto InspectVolumeTextureImportProvenance(
		const DVolumeTexture& Texture,
		AssetForge::FImportProvenance& OutProvenance,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SubmitVolumeTextureImport(
		std::string_view FilePath, const FAssetPath& Destination,
		const FVolumeTextureImportSettings& Settings, bool bAllowEngineContentWrite,
		AssetForge::FImportCompletion Completion,
		std::string& OutError)
		-> AssetForge::FImportHandle;
}
