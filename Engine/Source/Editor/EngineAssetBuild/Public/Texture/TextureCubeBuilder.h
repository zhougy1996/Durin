#pragma once

#include "EngineAssetBuildAPI.h"
#include "ImageDecoder.h"
#include "Texture/TextureCube.h"

namespace Durin::AssetBuild::TextureCubeBuilder
{
	inline constexpr uint64 MaximumPanoramaPixels = 32ull * 1024ull * 1024ull;
	inline constexpr uint32 MaximumPanoramaDimension = 16384;
	inline constexpr uint32 MaximumProjectedCubeFaceDimension = 4096;
	inline constexpr float MinimumPanoramaExposureEV = -16.0f;
	inline constexpr float MaximumPanoramaExposureEV = 16.0f;

	// Selects output resolution and the offline HDR exposure transform.
	struct FEquirectangularTextureCubeProjectionSettings
	{
		// Zero derives max(1, panorama width / 4).
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
	};

	// Validates the shared 2:1 and allocation contract and resolves the output face dimension.
	ENGINEASSETBUILD_API auto ValidateEquirectangularTextureCubeProjection(uint32 Width, uint32 Height,
		const FEquirectangularTextureCubeProjectionSettings& Settings, bool bHDR,
		uint32& OutFaceDimension, std::string& OutError) -> bool;

	// Projects top-left-origin sRGB RGBA8 panorama pixels into the canonical six-face source boundary.
	ENGINEASSETBUILD_API auto ProjectEquirectangularTextureCube(const Asset::FDecodedImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool;

	// Applies exposure and the fixed filmic curve while projecting a linear Radiance HDR panorama.
	ENGINEASSETBUILD_API auto ProjectEquirectangularTextureCube(const Asset::FDecodedFloatImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool;
}

