#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

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
		FTextureCubeFaceImages FaceImages;
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

	// LDR processing retains six faces and optionally the original panorama for saving.
	struct FTextureCubeLDRCanonicalInput
	{
		FTextureCubeFaceImages FaceImages;
		Image::FImage AuthoredPanorama;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::SixFaces;
		bool bSRGB = true;
	};

	// HDR processing retains a linear float panorama instead of RGBA8 scratch faces.
	struct FTextureCubeHDRCanonicalInput
	{
		Image::FImage AuthoredPanorama;
	};

	// Engine-owned canonical pixels and authoring metadata; output mode follows the
	// pixel alternative, so HDR cannot carry LDR faces or an sRGB flag.
	struct FTextureCubeCanonicalBuildInput
	{
		std::variant<FTextureCubeLDRCanonicalInput, FTextureCubeHDRCanonicalInput> Pixels;
		uint32 OriginalSourceWidth = 0;
		uint32 OriginalSourceHeight = 0;
		uint32 PanoramaFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;

		auto GetOutput() const -> ETextureCubeOutput
		{ return std::holds_alternative<FTextureCubeHDRCanonicalInput>(Pixels) ? ETextureCubeOutput::HDR : ETextureCubeOutput::LDR; }
		auto GetSourceLayout() const -> ETextureCubeSourceLayout
		{
			const auto* LDR = std::get_if<FTextureCubeLDRCanonicalInput>(&Pixels);
			return LDR ? LDR->SourceLayout : ETextureCubeSourceLayout::EquirectangularPanorama;
		}
		auto GetSRGB() const -> bool
		{
			const auto* LDR = std::get_if<FTextureCubeLDRCanonicalInput>(&Pixels);
			return LDR && LDR->bSRGB;
		}
		auto GetAuthoredPanorama() const -> const Image::FImage&
		{ return std::visit([](const auto& Input) -> const Image::FImage& { return Input.AuthoredPanorama; }, Pixels); }
	};

	// Recipe alternatives borrow immutable pixels for one synchronous invocation.
	struct FTextureCubeLDRBuildInput
	{
		std::reference_wrapper<const FTextureCubeFaceImages> FaceImages;
		bool bSRGB = true;
	};

	struct FTextureCubeHDRBuildInput
	{
		std::reference_wrapper<const Image::FImage> Panorama;
		uint32 FaceDimension = 0;
		float ExposureEV = 0.0f;
	};

	struct FTextureCubeBuildInput
	{
		std::variant<FTextureCubeLDRBuildInput, FTextureCubeHDRBuildInput> Pixels;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

}

#endif
