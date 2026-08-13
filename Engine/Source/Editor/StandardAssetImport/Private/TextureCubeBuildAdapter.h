#pragma once

#include "Texture/TextureCubeBuildOperations.h"

namespace Durin::StandardAssetImport
{
	inline auto BuildAndPublishTextureCubePanorama(
		DTextureCube& Texture,
		AssetBuild::TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		AssetBuild::FTextureCubeBuildProduct Product;
		if (!AssetBuild::BuildTextureCubePanorama(
			std::move(Panorama), SourceHash, Settings, Product, OutError)) return false;
		return AssetBuild::PublishTextureCubeProduct(Texture, std::move(Product), {
			.PanoramaHash = SourceHash,
			.PanoramaPath = SourcePath}, OutError);
	}

	inline auto BuildAndPublishTextureCubePanorama(
		DTextureCube& Texture,
		AssetBuild::TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		AssetBuild::FTextureCubeBuildProduct Product;
		if (!AssetBuild::BuildTextureCubePanorama(
			std::move(Panorama), SourceHash, Settings, Product, OutError)) return false;
		return AssetBuild::PublishTextureCubeProduct(Texture, std::move(Product), {
			.PanoramaHash = SourceHash,
			.PanoramaPath = SourcePath}, OutError);
	}

	inline auto BuildAndPublishTextureCubeFaces(
		DTextureCube& Texture,
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& SourceHashes,
		const std::array<FSourcePath, TextureCubeFaceCount>& SourcePaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		AssetBuild::FTextureCubeBuildProduct Product;
		if (!AssetBuild::BuildTextureCubeFaces(
			std::move(SourceData), SourceHashes, Settings, Product, OutError)) return false;
		return AssetBuild::PublishTextureCubeProduct(Texture, std::move(Product), {
			.FaceHashes = SourceHashes,
			.FacePaths = SourcePaths}, OutError);
	}
}
