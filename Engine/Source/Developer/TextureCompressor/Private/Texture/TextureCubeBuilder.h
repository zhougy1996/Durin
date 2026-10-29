#pragma once

#include "CoreMinimal.h"

#include "TextureCompressorAPI.h"
#include "Texture/TextureCubeValidation.h"
#include "Texture/TextureCubeBuildTypes.h"

namespace Durin::TextureCubeBuilder
{
	// Validates linear radiance and the bounded HDR output envelope before allocation.
	TEXTURECOMPRESSOR_API auto ValidateHDRTextureCubePanorama(const FTextureCubePanoramaFloatImage& Panorama,
		const FTextureCubePanoramaBuildSettings& Settings) -> std::expected<void, FTextureCompressorDiagnostic>;
	// Builds ordinary radiance mips directly from the retained panorama, without RGBA8 scratch data.
	TEXTURECOMPRESSOR_API auto BuildHDRTextureCube(const Image::FImage& Panorama,
		const FTextureCubePanoramaBuildSettings& Settings) -> std::expected<FTextureCubePlatformData, std::string>;

	// Selects output resolution and the offline HDR exposure transform.
	struct FEquirectangularTextureCubeProjectionSettings
	{
		// Zero derives max(1, panorama width / 4).
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
	};

	// Validates the shared 2:1 and allocation contract and resolves the output face dimension.
	TEXTURECOMPRESSOR_API auto ValidateEquirectangularTextureCubeProjection(uint32 Width, uint32 Height,
		const FEquirectangularTextureCubeProjectionSettings& Settings, bool bHDR,
		uint32& OutFaceDimension) -> std::expected<void, FTextureCompressorDiagnostic>;

	// Projects top-left-origin sRGB RGBA8 panorama pixels into the canonical six-face source boundary.
	TEXTURECOMPRESSOR_API auto ProjectEquirectangularTextureCube(const FTextureCubePanoramaImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeFaceImages& OutSourceData) -> std::expected<void, FTextureCompressorDiagnostic>;

	// Applies exposure and the fixed filmic curve while projecting a linear Radiance HDR panorama.
	TEXTURECOMPRESSOR_API auto ProjectEquirectangularTextureCube(const FTextureCubePanoramaFloatImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeFaceImages& OutSourceData) -> std::expected<void, FTextureCompressorDiagnostic>;
}
