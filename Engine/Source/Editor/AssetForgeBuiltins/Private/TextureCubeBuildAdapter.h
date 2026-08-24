#pragma once

#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	inline auto BuildAndPublishTextureCubePanorama(
		DTextureCube& Texture,
		Asset::Build::TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		Asset::Build::FTextureCubeBuildProduct Product;
		if (!Asset::Build::BuildTextureCubePanorama(
			std::move(Panorama), SourceHash, Settings, Product, OutError)) return false;
		return Asset::Build::PublishTextureCubeProduct(Texture, std::move(Product), {
			.PanoramaHash = SourceHash,
			.PanoramaPath = SourcePath}, OutError);
	}

	inline auto BuildAndPublishTextureCubePanorama(
		DTextureCube& Texture,
		Asset::Build::TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		Asset::Build::FTextureCubeBuildProduct Product;
		if (!Asset::Build::BuildTextureCubePanorama(
			std::move(Panorama), SourceHash, Settings, Product, OutError)) return false;
		return Asset::Build::PublishTextureCubeProduct(Texture, std::move(Product), {
			.PanoramaHash = SourceHash,
			.PanoramaPath = SourcePath}, OutError);
	}

	inline auto BuildAndPublishTextureCubePanorama(
		DTextureCube& Texture,
		FTextureCubePanoramaSourceData Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		return std::visit([&](auto&& Source) {
			return BuildAndPublishTextureCubePanorama(
				Texture, std::move(Source), SourceHash, SourcePath, Settings, OutError);
		}, std::move(Panorama));
	}

	inline auto BuildAndPublishTextureCubeFaces(
		DTextureCube& Texture,
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& SourceHashes,
		const std::array<FSourcePath, TextureCubeFaceCount>& SourcePaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		Asset::Build::FTextureCubeBuildProduct Product;
		if (!Asset::Build::BuildTextureCubeFaces(
			std::move(SourceData), SourceHashes, Settings, Product, OutError)) return false;
		return Asset::Build::PublishTextureCubeProduct(Texture, std::move(Product), {
			.FaceHashes = SourceHashes,
			.FacePaths = SourcePaths}, OutError);
	}
}
