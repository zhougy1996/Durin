#pragma once

#include "TextureBuildAPI.h"
#include "Texture/TextureCubeBuildProvider.h"

namespace Durin::TextureCubeBuilder
{
	inline constexpr uint64 MaximumPanoramaPixels = MaximumTextureCubePanoramaPixels;
	inline constexpr uint32 MaximumPanoramaDimension = MaximumTextureCubePanoramaDimension;
	inline constexpr uint32 MaximumProjectedCubeFaceDimension = MaximumProjectedTextureCubeFaceDimension;
	inline constexpr float MinimumPanoramaExposureEV = MinimumTextureCubePanoramaExposureEV;
	inline constexpr float MaximumPanoramaExposureEV = MaximumTextureCubePanoramaExposureEV;

	using FTexturePanoramaImage = FTextureCubePanoramaImage;
	using FTexturePanoramaFloatImage = FTextureCubePanoramaFloatImage;

	// Selects output resolution and the offline HDR exposure transform.
	struct FEquirectangularTextureCubeProjectionSettings
	{
		// Zero derives max(1, panorama width / 4).
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
	};

	// Validates the shared 2:1 and allocation contract and resolves the output face dimension.
	TEXTUREBUILD_API auto ValidateEquirectangularTextureCubeProjection(uint32 Width, uint32 Height,
		const FEquirectangularTextureCubeProjectionSettings& Settings, bool bHDR,
		uint32& OutFaceDimension, std::string& OutError) -> bool;

	// Projects top-left-origin sRGB RGBA8 panorama pixels into the canonical six-face source boundary.
	TEXTUREBUILD_API auto ProjectEquirectangularTextureCube(const FTexturePanoramaImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool;

	// Applies exposure and the fixed filmic curve while projecting a linear Radiance HDR panorama.
	TEXTUREBUILD_API auto ProjectEquirectangularTextureCube(const FTexturePanoramaFloatImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool;
}
