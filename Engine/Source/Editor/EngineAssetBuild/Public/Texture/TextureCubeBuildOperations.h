#pragma once

#include "EngineAssetBuildAPI.h"
#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "Texture/TextureCube.h"

namespace Durin::AssetBuild
{
	ENGINEASSETBUILD_API auto BuildTextureCubePanorama(
		DTextureCube& Texture,
		Asset::FDecodedImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ENGINEASSETBUILD_API auto BuildTextureCubePanorama(
		DTextureCube& Texture,
		Asset::FDecodedFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ENGINEASSETBUILD_API auto BuildTextureCubeFaces(
		DTextureCube& Texture,
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& SourceHashes,
		const std::array<FSourcePath, TextureCubeFaceCount>& SourcePaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool;

	// Transitional encoded adapters retained only for legacy Runtime TextureCube
	// authoring callers; StandardAssetImport uses the normalized overloads above.
	ENGINEASSETBUILD_API auto BuildTextureCubePanoramaFromEncodedBytes(
		DTextureCube& Texture,
		std::span<const uint8> EncodedBytes,
		std::string_view ExtensionHint,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;

	ENGINEASSETBUILD_API auto BuildTextureCubeFacesFromEncodedBytes(
		DTextureCube& Texture,
		const std::array<std::span<const uint8>, TextureCubeFaceCount>& EncodedFaces,
		const std::array<FSourcePath, TextureCubeFaceCount>& SourcePaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool;
}
