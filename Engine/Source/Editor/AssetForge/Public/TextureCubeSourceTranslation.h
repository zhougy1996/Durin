#pragma once

#include "AssetForgeAPI.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"

namespace Durin::Asset::Forge
{
	using FTextureCubeImportSettings = Asset::Build::FTextureCubeFacesBuildSettings;
	using FTextureCubePanoramaImportSettings =
		Asset::Build::FTextureCubePanoramaBuildSettings;
	using FTextureCubePanoramaSourceData = std::variant<
		Asset::Build::TextureCubeBuilder::FTexturePanoramaImage,
		Asset::Build::TextureCubeBuilder::FTexturePanoramaFloatImage>;
	ASSETFORGE_API auto IsTextureCubeFaceSourceExtension(
		std::string_view Extension) -> bool;
	ASSETFORGE_API auto IsTextureCubePanoramaSourceExtension(
		std::string_view Extension) -> bool;

	ASSETFORGE_API auto TranslateTextureCubePanoramaSource(
		std::span<const uint8> EncodedBytes,
		std::string_view ExtensionHint,
		FTextureCubePanoramaSourceData& OutSource,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto TranslateTextureCubeFaceSources(
		const std::array<std::span<const uint8>, TextureCubeFaceCount>& EncodedFaces,
		FTextureCubeSourceData& OutSource,
		std::string& OutError) -> bool;

	struct FTextureCubeImportValidation
	{
		bool bValid = false;
		std::string Message;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		uint32 SourceWidth = 0;
		uint32 SourceHeight = 0;
		uint32 Dimension = 0;
		uint32 MipCount = 0;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		bool bHDR = false;

		explicit operator bool() const { return bValid; }
	};

	struct FTextureCubeImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DTextureCube* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};

	ASSETFORGE_API auto ValidateTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings = {})
		-> FTextureCubeImportValidation;
	ASSETFORGE_API auto ValidateTextureCubePanorama(
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings = {})
		-> FTextureCubeImportValidation;
	ASSETFORGE_API auto ImportTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		std::string_view AssetPath,
		const FTextureCubeImportSettings& Settings = {},
		const std::array<std::string, TextureCubeFaceCount>& SourceDestinations = {},
		bool bEngineAuthoringContext = false) -> FTextureCubeImportResult;
	ASSETFORGE_API auto ImportTextureCubePanorama(
		std::string_view PanoramaFile,
		std::string_view AssetPath,
		const FTextureCubePanoramaImportSettings& Settings = {},
		std::string_view SourceDestination = {},
		bool bEngineAuthoringContext = false) -> FTextureCubeImportResult;
	ASSETFORGE_API auto ReimportTextureCubePanorama(
		DTextureCube& Texture,
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto ReimportTextureCubeFaces(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto ChangeTextureCubePanoramaSourceReference(
		DTextureCube& Texture,
		std::string_view SourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto ChangeTextureCubeFaceSourceReferences(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& SourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto IngestAndChangeTextureCubePanoramaSource(
		DTextureCube& Texture,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto IngestAndChangeTextureCubeFaceSources(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const std::array<std::string, TextureCubeFaceCount>& TargetSourceVirtualPaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool;
}
