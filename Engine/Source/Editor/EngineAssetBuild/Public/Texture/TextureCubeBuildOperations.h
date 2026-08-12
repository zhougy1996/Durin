#pragma once

#include "EngineAssetBuildAPI.h"
#include "Texture/TextureCube.h"

namespace Durin::AssetBuild
{
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
