#pragma once

#include "Texture/TextureCubeData.h"
#include "Texture/TextureBuildOutcome.h"

namespace Durin
{
	inline constexpr uint64 MaximumTextureCubePanoramaPixels = 32ull * 1024ull * 1024ull;
	inline constexpr uint32 MaximumTextureCubePanoramaDimension = 16384;
	inline constexpr uint32 MaximumProjectedTextureCubeFaceDimension = 4096;
	inline constexpr float MinimumTextureCubePanoramaExposureEV = -16.0f;
	inline constexpr float MaximumTextureCubePanoramaExposureEV = 16.0f;

	struct FTextureCubeFacesBuildSettings
	{
		bool bSRGB = true;
	};

	struct FTextureCubePanoramaBuildSettings
	{
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
		ETextureCubeOutput Output = ETextureCubeOutput::LDR;
	};

	struct FTextureCubePanoramaImage
	{
		FByteBuffer Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		bool bHasTransparency = false;
	};

	struct FTextureCubePanoramaFloatImage
	{
		std::vector<float> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
	};

	// Canonical faces may retain panorama authoring metadata when used by PostLoad.
	struct FTextureCubeFacesBuildInput
	{
		FTextureCubeDecodedFaces DecodedFaces;
		// Installed source identity for rebuilds; imports let Engine prepare the canonical identity.
		FXxHash128 SourceIdentity;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		uint32 OriginalSourceWidth = 0;
		uint32 OriginalSourceHeight = 0;
		uint32 PanoramaFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		FTextureCubeFacesBuildSettings Settings;
	};

	struct FTextureCubePanoramaBuildInput
	{
		std::variant<FTextureCubePanoramaImage, FTextureCubePanoramaFloatImage> Image;
		FTextureCubePanoramaBuildSettings Settings;
	};

	using FTextureCubeNormalizationInput = std::variant<FTextureCubeFacesBuildInput, FTextureCubePanoramaBuildInput>;

	// Borrows immutable pixels only for this synchronous module invocation.
	struct FTextureCubeNormalizeRequest
	{
		std::reference_wrapper<const FTextureCubeNormalizationInput> Input;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

	// Engine-owned canonical authoring state produced while normalizing panorama input.
	struct FTextureCubeCanonicalBuildInput
	{
		FTextureCubeDecodedFaces DecodedFaces;
		// Installed source identity for rebuilds; imports let Engine prepare the canonical identity.
		FXxHash128 SourceIdentity;
		Image::FImage AuthoredPanorama;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		uint32 OriginalSourceWidth = 0;
		uint32 OriginalSourceHeight = 0;
		uint32 PanoramaFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		bool bSRGB = true;
		ETextureCubeOutput Output = ETextureCubeOutput::LDR;
	};

	struct FTextureCubeBuildInput
	{
		std::reference_wrapper<const FTextureCubeDecodedFaces> DecodedFaces;
		bool bSRGB = true;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		// Borrowed only for this synchronous invocation; HDR bypasses RGBA8 scratch faces.
		const Image::FImage* HDRPanorama = nullptr;
		FTextureCubePanoramaBuildSettings PanoramaSettings;
	};

}
