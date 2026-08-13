#pragma once

#include "EngineAssetBuildAPI.h"
#include "Hash/XxHash.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuilder.h"

namespace Durin::AssetBuild
{
	ENGINEASSETBUILD_API auto BuildTextureCubePanorama(
		DTextureCube& Texture,
		TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool;
	ENGINEASSETBUILD_API auto BuildTextureCubePanorama(
		DTextureCube& Texture,
		TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
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
	ENGINEASSETBUILD_API auto MakeTextureCubeDerivedDataKey(
		const DTextureCube& Texture,
		std::string& OutError) -> std::string;
	ENGINEASSETBUILD_API auto LoadTextureCubeDerivedData(
		std::string_view Key,
		std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool;

}
