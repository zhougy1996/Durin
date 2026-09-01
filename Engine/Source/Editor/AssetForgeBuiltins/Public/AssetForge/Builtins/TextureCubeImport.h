#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	using FTextureCubeImportSettings = FTextureCubeFacesBuildSettings;
	using FTextureCubePanoramaImportSettings =
		FTextureCubePanoramaBuildSettings;
	using FTextureCubePanoramaSourceData = std::variant<
		TextureCubeBuilder::FTexturePanoramaImage,
		TextureCubeBuilder::FTexturePanoramaFloatImage>;
	ASSETFORGEBUILTINS_API auto IsTextureCubeFaceSourceExtension(
		std::string_view Extension) -> bool;
	ASSETFORGEBUILTINS_API auto IsTextureCubePanoramaSourceExtension(
		std::string_view Extension) -> bool;

	ASSETFORGEBUILTINS_API auto TranslateTextureCubePanoramaSource(
		std::span<const std::byte> EncodedBytes,
		std::string_view ExtensionHint,
		FTextureCubePanoramaSourceData& OutSource,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto TranslateTextureCubeFaceSources(
		const std::array<std::span<const std::byte>, TextureCubeFaceCount>& EncodedFaces,
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

	ASSETFORGEBUILTINS_API auto ValidateTextureCubeFaces(
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings = {})
		-> FTextureCubeImportValidation;
	ASSETFORGEBUILTINS_API auto ValidateTextureCubePanorama(
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings = {})
		-> FTextureCubeImportValidation;
	ASSETFORGEBUILTINS_API auto ReimportTextureCubePanorama(
		DTextureCube& Texture,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ReimportTextureCubePanoramaFromFile(
		DTextureCube& Texture,
		std::string_view PanoramaFile,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ReimportTextureCubeFaces(
		DTextureCube& Texture,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ReimportTextureCubeFacesFromFile(
		DTextureCube& Texture,
		const std::array<std::string, TextureCubeFaceCount>& FaceFiles,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool;
}
