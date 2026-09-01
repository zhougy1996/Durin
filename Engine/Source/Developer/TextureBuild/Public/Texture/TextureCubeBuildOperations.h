#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuilder.h"

namespace Durin
{
	struct FTextureCubeFacesBuildSettings
	{
		bool bSRGB = true;
	};

	struct FTextureCubePanoramaBuildSettings
	{
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
	};

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
		// Cache hits intentionally omit transient normalized source pixels.
		bool bLoadedFromDerivedDataCache = false;
		std::string PersistenceDiagnostic;
	};

	// GameThread provenance captured outside the pure TextureCube recipe.
	struct FTextureCubePublicationContext
	{
		FXxHash128 PanoramaHash;
		std::array<FXxHash128, TextureCubeFaceCount> FaceHashes{};
	};

	TEXTUREBUILD_API auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCubeFaces(
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& SourceHashes,
		const FTextureCubeFacesBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCubePanoramaInto(
		DTextureCube& Texture,
		TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCubePanoramaInto(
		DTextureCube& Texture,
		TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		std::string& OutError) -> bool;
	TEXTUREBUILD_API auto BuildTextureCubeFacesInto(
		DTextureCube& Texture,
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& SourceHashes,
		const FTextureCubeFacesBuildSettings& Settings,
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
