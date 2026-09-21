#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/PackageSerialization.h"
#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Asset/SourceHint.h"
#include "Misc/MountPaths.h"
#include "Texture/TextureBuildOutcome.h"
#include "Texture/VolumeTexture.h"

namespace Durin { struct FAssetImportDataError; }

namespace Durin::AssetForge::Builtins
{
	struct FVolumeTextureCapturedSource
	{
		std::string Filename;
		FXxHash128 ContentHash{};
		FByteView Bytes;
	};

	struct FVolumeTextureImportSettingsResult;

	struct FVolumeTextureImportSettings
	{
		EVolumeTextureImportFormat ImportFormat = EVolumeTextureImportFormat::PngRowMajorAtlas;
		EVolumeTextureSourceChannels Channels = EVolumeTextureSourceChannels::Red;
		uint32 SliceWidth = 128;
		uint32 SliceHeight = 128;
		uint32 Depth = 128;
		uint32 TilesX = 12;
		uint32 TilesY = 12;

		ASSETFORGEBUILTINS_API auto Validate() const -> FVolumeTextureImportSettingsResult;
		auto GetOutputFormat() const -> EVolumeTextureFormat
		{
			return Channels == EVolumeTextureSourceChannels::RGBA
				? EVolumeTextureFormat::RGBA8_UNORM
				: EVolumeTextureFormat::R8_UNORM;
		}
	};

	enum class EVolumeTextureImportSettingsError : uint8
	{
		None, ImportFormat, Dimensions, CellCount, AtlasBudget, VolumeBudget
	};
	struct FVolumeTextureImportSettingsError
	{
		EVolumeTextureImportSettingsError Code = EVolumeTextureImportSettingsError::None;
		FVolumeTextureImportSettings Settings;
	};
	struct FVolumeTextureImportSettingsResult
	{
		FVolumeTextureImportSettingsError Error;
		explicit operator bool() const { return Error.Code == EVolumeTextureImportSettingsError::None; }
	};
	ASSETFORGEBUILTINS_API auto FormatVolumeTextureImportSettingsError(
		const FVolumeTextureImportSettingsError& Error) -> std::string;

	enum class EVolumeTextureTranslationError : uint8
	{
		None, Settings, Signature, Decode, Dimensions, Publication, Layout
	};
	struct FVolumeTextureTranslationError
	{
		EVolumeTextureTranslationError Code = EVolumeTextureTranslationError::None;
		std::string Filename;
		uint64 ExpectedWidth = 0;
		uint64 ExpectedHeight = 0;
		uint32 ActualWidth = 0;
		uint32 ActualHeight = 0;
		std::optional<FVolumeTextureImportSettingsError> SettingsCause;
		std::optional<Image::FImageDecodeError> DecodeCause;
	};
	struct FVolumeTextureTranslationResult
	{
		FVolumeTextureTranslationError Error;
		explicit operator bool() const { return Error.Code == EVolumeTextureTranslationError::None; }
	};
	ASSETFORGEBUILTINS_API auto FormatVolumeTextureTranslationError(
		const FVolumeTextureTranslationError& Error) -> std::string;

	struct FEncodedSourceError;
	enum class EVolumeTextureRebuildError : uint8
	{
		None, Package, Mount, SourceHint, SourceFile, Capture, Translation,
		Build, ImportValidation, ImportAllocation, Save, ImportData, MissingSource
	};
	struct FVolumeTextureRebuildError
	{
		EVolumeTextureRebuildError Code = EVolumeTextureRebuildError::None;
		std::string ObjectPath;
		std::string Filename;
		EMountPathError MountCause = EMountPathError::None;
		std::optional<FSourceHintError> SourceHintCause;
		std::shared_ptr<const FEncodedSourceError> CaptureCause;
		std::optional<FVolumeTextureTranslationError> TranslationCause;
		std::optional<FTextureBuildOutcome> BuildCause;
		std::shared_ptr<const FAssetImportDataError> ImportCause;
		std::shared_ptr<const FAssetWriteResult> SaveCause;
	};
	struct FVolumeTextureRebuildResult
	{
		FVolumeTextureRebuildError Error;
		explicit operator bool() const { return Error.Code == EVolumeTextureRebuildError::None; }
	};
	ASSETFORGEBUILTINS_API auto FormatVolumeTextureRebuildError(const FVolumeTextureRebuildError& Error) -> std::string;

	// Describes source-derived import suggestions without relying on file naming.
	struct FVolumeTextureAtlasInspection
	{
		Image::FImageDecodeError Error;
		bool bHasConfidentLayout = false;
		uint32 AtlasWidth = 0;
		uint32 AtlasHeight = 0;
		uint8 SourceChannelCount = 0;
		EVolumeTextureSourceChannels SuggestedChannels =
			EVolumeTextureSourceChannels::Red;
		std::vector<FVolumeTextureImportSettings> SuggestedLayouts;

		explicit operator bool() const { return Error.Code == Image::EImageDecodeError::None; }
	};

	ASSETFORGEBUILTINS_API auto FormatVolumeTextureAtlasInspection(
		const FVolumeTextureAtlasInspection& Inspection) -> std::string;
	ASSETFORGEBUILTINS_API auto InspectVolumeTextureAtlasSource(
		std::string_view FilePath) -> FVolumeTextureAtlasInspection;
	ASSETFORGEBUILTINS_API auto TranslateVolumeTextureAtlasSource(
		const FVolumeTextureCapturedSource& Source,
		const FVolumeTextureImportSettings& Settings,
		FVolumeTextureSourceData& OutSourceData) -> FVolumeTextureTranslationResult;
	ASSETFORGEBUILTINS_API auto ReimportVolumeTexture(
		DVolumeTexture& Texture,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> FVolumeTextureRebuildResult;
	ASSETFORGEBUILTINS_API auto ReimportVolumeTextureFromFile(
		DVolumeTexture& Texture,
		std::string_view FilePath,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> FVolumeTextureRebuildResult;
}
