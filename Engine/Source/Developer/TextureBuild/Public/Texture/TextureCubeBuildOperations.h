#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuilder.h"

namespace Durin::AssetBuild
{
	// Detached TextureCube recipe output; publication is a separate GameThread operation.
	struct FTextureCubeBuildProduct
	{
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		FTextureCubeSourceData SourceData;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		std::string DerivedDataKey;
		uint32 SourceWidth = 0;
		uint32 SourceHeight = 0;
		uint32 PanoramaFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		bool bSRGB = true;
	};

	// GameThread provenance captured outside the pure TextureCube recipe.
	struct FTextureCubePublicationContext
	{
		FXxHash128 PanoramaHash;
		FSourcePath PanoramaPath;
		std::array<FXxHash128, TextureCubeFaceCount> FaceHashes{};
		std::array<FSourcePath, TextureCubeFaceCount> FacePaths{};
		std::string DecoderId = "DurinImage";
		uint32 DecoderVersion = 1;
	};

	TEXTUREBUILD_API auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaImportSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaImportSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCubeFaces(
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& SourceHashes,
		const FTextureCubeImportSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto PublishTextureCubeProduct(
		DTextureCube& Texture,
		FTextureCubeBuildProduct Product,
		const FTextureCubePublicationContext& Context,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto MakeTextureCubeDerivedDataKey(
		const DTextureCube& Texture,
		std::string& OutError) -> std::string;
	TEXTUREBUILD_API auto LoadTextureCubeDerivedData(
		std::string_view Key,
		std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool;

}
