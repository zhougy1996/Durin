#include "Texture/TextureCubeBuildOperations.h"

#include "Texture/TextureBuilder.h"
#include "Texture/TextureCubeBuilder.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
	}

	auto NormalizeTextureCube(const FTextureCubeBuildRequest& Request,
		FTextureCubeCanonicalBuildInput& OutCanonicalInput,
		std::string& OutError) -> bool
	{
		OutCanonicalInput = {};
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			OutError = "TextureCube build target is unsupported.";
			return false;
		}
		if (const auto* Faces = std::get_if<FTextureCubeFacesBuildInput>(&Request.Input))
		{
			if (!Faces->DecodedFaces.IsValid())
			{
				OutError = "TextureCube decoded faces are invalid.";
				return false;
			}
			OutCanonicalInput = {.DecodedFaces = Faces->DecodedFaces,
				.SourceIdentity = Faces->SourceIdentity,
				.SourceLayout = Faces->SourceLayout,
				.OriginalSourceWidth = Faces->OriginalSourceWidth,
				.OriginalSourceHeight = Faces->OriginalSourceHeight,
				.PanoramaFaceDimension = Faces->PanoramaFaceDimension,
				.PanoramaExposureEV = Faces->PanoramaExposureEV,
				.bSRGB = Faces->Settings.bSRGB};
			OutError.clear();
			return true;
		}

		const auto& Panorama = std::get<FTextureCubePanoramaBuildInput>(Request.Input);
		if (Panorama.Settings.Output != ETextureCubeOutput::LDR
			&& Panorama.Settings.Output != ETextureCubeOutput::HDR)
		{
			OutError = "TextureCube output range is invalid.";
			return false;
		}
		return std::visit([&](const auto& Image) {
			FTextureCubeDecodedFaces SourceData;
			const bool bHDR = Panorama.Settings.Output == ETextureCubeOutput::HDR;
			if (bHDR)
			{
				if constexpr (std::is_same_v<std::decay_t<decltype(Image)>, FTextureCubePanoramaFloatImage>)
				{
					if (!TextureCubeBuilder::ValidateHDRTextureCubePanorama(Image, Panorama.Settings, OutError)) return false;
				}
				else
				{
					OutError = "HDR cube output requires a linear float panorama.";
					return false;
				}
			}
			else if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
				Image, {Panorama.Settings.FaceDimension, Panorama.Settings.ExposureEV},
				SourceData, OutError)) return false;
			if (!bHDR && !SourceData.IsValid())
			{
				OutError = "TextureCube decoded faces are invalid.";
				return false;
			}
			Durin::Image::FImage AuthoredPanorama;
			const Durin::Image::FImageInfo Info{.Width = Image.Width,
				.Height = Image.Height,
				.Format = std::is_same_v<std::decay_t<decltype(Image)>,
					FTextureCubePanoramaFloatImage>
					? Durin::Image::ERawImageFormat::RGBA32F
					: Durin::Image::ERawImageFormat::RGBA8,
				.GammaSpace = std::is_same_v<std::decay_t<decltype(Image)>,
					FTextureCubePanoramaFloatImage>
					? Durin::Image::EImageGammaSpace::Linear
					: Durin::Image::EImageGammaSpace::SRGB};
			FByteBuffer AuthoredBytes;
			if constexpr (std::is_same_v<std::decay_t<decltype(Image)>,
				FTextureCubePanoramaFloatImage>)
			{
				const size_t PixelCount = static_cast<size_t>(Image.Width) * Image.Height;
				AuthoredBytes.resize(PixelCount * 4 * sizeof(float));
				for (size_t Index = 0; Index < PixelCount; ++Index)
				{
					std::memcpy(AuthoredBytes.data() + Index * 4 * sizeof(float),
						Image.Pixels.data() + Index * 3, 3 * sizeof(float));
					const float Alpha = 1.0f;
					std::memcpy(AuthoredBytes.data() + (Index * 4 + 3) * sizeof(float),
						&Alpha, sizeof(float));
				}
			}
			else
			{
				AuthoredBytes.assign(Image.Pixels.begin(), Image.Pixels.end());
			}
			if (!Durin::Image::FImage::TryCreate(Info,
				std::move(AuthoredBytes), AuthoredPanorama, &OutError)) return false;
			OutCanonicalInput = {.DecodedFaces = std::move(SourceData),
				.AuthoredPanorama = std::move(AuthoredPanorama),
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.OriginalSourceWidth = Image.Width,
				.OriginalSourceHeight = Image.Height,
				.PanoramaFaceDimension = Panorama.Settings.FaceDimension,
				.PanoramaExposureEV = Panorama.Settings.ExposureEV,
				.bSRGB = !bHDR, .Output = Panorama.Settings.Output};
			OutError.clear();
			return true;
		}, Panorama.Image);
	}

	auto BuildTextureCube(const FTextureCubeRecipeBuildRequest& Request,
		FTextureCubeRecipeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (Request.HDRPanorama != nullptr)
		{
			if (Request.TargetPlatform != ECookTargetPlatform::Win64
				|| Request.TargetProfile != ECookTargetProfile::Game || Request.bSRGB)
			{
				OutError = "HDR cube build target or color space is invalid.";
				return false;
			}
			auto PlatformData = std::make_unique<FTextureCubePlatformData>();
			if (!TextureCubeBuilder::BuildHDRTextureCube(*Request.HDRPanorama,
				Request.PanoramaSettings, *PlatformData, OutError)) return false;
			OutProduct.PlatformData = std::move(PlatformData);
			return true;
		}
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| !Request.DecodedFaces.get().IsValid())
		{
			OutError = "TextureCube canonical build request is invalid.";
			return false;
		}
		const FTextureCubeDecodedFaces& SourceData = Request.DecodedFaces.get();
		const bool bHasTransparency = SourceData.TransparencyMask != 0;
		auto PlatformData = std::make_unique<FTextureCubePlatformData>();
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const FTexture2DBuildResult BuildResult = TextureBuilder::BuildMipChain(
				std::span(&SourceData.Faces[Index], 1), ETextureUsage::Color,
				Request.bSRGB, PlatformData->Faces[Index], 0,
				ETextureCompressionQuality::Normal, ETextureAlphaMipMode::Average,
				0.5f, nullptr, bHasTransparency);
			if (!BuildResult)
			{
				OutError = std::format("{} face platform build failed: {}",
					FaceNames[Index], BuildResult.Diagnostic);
				return false;
			}
		}
		PlatformData->PixelFormat = PlatformData->Faces[0].PixelFormat;
		if (!PlatformData->IsValid())
		{
			OutError = "Cube texture platform data is inconsistent.";
			return false;
		}
		OutProduct.PlatformData = std::move(PlatformData);
		OutError.clear();
		return true;
	}
}
