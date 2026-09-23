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

	auto NormalizeTextureCube(const FTextureCubeNormalizeRequest& Request) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError>
	{
		FTextureCubeCanonicalBuildInput CanonicalInput;
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"TextureCube build target is unsupported."});
		}
		if (const auto* Faces = std::get_if<FTextureCubeFacesBuildInput>(&Request.Input.get()))
		{
			if (!Faces->DecodedFaces.IsValid())
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					"TextureCube decoded faces are invalid."});
			}
			if ((Faces->SourceLayout != ETextureCubeSourceLayout::SixFaces
				&& Faces->SourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama)
				|| !std::isfinite(Faces->PanoramaExposureEV)
				|| Faces->PanoramaExposureEV < MinimumTextureCubePanoramaExposureEV
				|| Faces->PanoramaExposureEV > MaximumTextureCubePanoramaExposureEV
				|| Faces->OriginalSourceWidth == 0 || Faces->OriginalSourceHeight == 0)
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput,
					ETextureBuildStage::Normalize, "TextureCube source layout, dimensions, or exposure are invalid."});
			CanonicalInput = {.DecodedFaces = Faces->DecodedFaces,
				.SourceIdentity = Faces->SourceIdentity,
				.SourceLayout = Faces->SourceLayout,
				.OriginalSourceWidth = Faces->OriginalSourceWidth,
				.OriginalSourceHeight = Faces->OriginalSourceHeight,
				.PanoramaFaceDimension = Faces->PanoramaFaceDimension,
				.PanoramaExposureEV = Faces->PanoramaExposureEV,
				.bSRGB = Faces->Settings.bSRGB};
			return CanonicalInput;
		}

		const auto& Panorama = std::get<FTextureCubePanoramaBuildInput>(Request.Input.get());
		if (Panorama.Settings.Output != ETextureCubeOutput::LDR
			&& Panorama.Settings.Output != ETextureCubeOutput::HDR)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"TextureCube output range is invalid."});
		}
		return std::visit([&](const auto& Image) -> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> {
			FTextureCubeDecodedFaces SourceData;
			const bool bHDR = Panorama.Settings.Output == ETextureCubeOutput::HDR;
			if (bHDR)
			{
				if constexpr (std::is_same_v<std::decay_t<decltype(Image)>, FTextureCubePanoramaFloatImage>)
				{
					if (auto Result = TextureCubeBuilder::ValidateHDRTextureCubePanorama(Image, Panorama.Settings); !Result) return std::unexpected(std::move(Result.error()));
				}
				else
				{
					return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
						"HDR cube output requires a linear float panorama."});
				}
			}
			else if (auto Result = TextureCubeBuilder::ProjectEquirectangularTextureCube(
				Image, {Panorama.Settings.FaceDimension, Panorama.Settings.ExposureEV},
				SourceData); !Result) return std::unexpected(std::move(Result.error()));
			if (!bHDR && !SourceData.IsValid())
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					"TextureCube decoded faces are invalid."});
			}
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
			auto ImageResult1 = Durin::Image::FImage::TryCreate(Info, std::move(AuthoredBytes));
			if (!ImageResult1)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					ImageResult1.error().ToString()});
			}
			auto AuthoredPanorama = std::move(*ImageResult1);
			CanonicalInput = {.DecodedFaces = std::move(SourceData),
				.AuthoredPanorama = std::move(AuthoredPanorama),
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.OriginalSourceWidth = Image.Width,
				.OriginalSourceHeight = Image.Height,
				.PanoramaFaceDimension = Panorama.Settings.FaceDimension,
				.PanoramaExposureEV = Panorama.Settings.ExposureEV,
				.bSRGB = !bHDR, .Output = Panorama.Settings.Output};
			return std::move(CanonicalInput);
		}, Panorama.Image);
	}

	auto BuildTextureCube(const FTextureCubeBuildInput& Request) -> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError>
	{
		if (Request.HDRPanorama != nullptr)
		{
			if (Request.TargetPlatform != ECookTargetPlatform::Win64
				|| Request.TargetProfile != ECookTargetProfile::Game || Request.bSRGB)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build,
					"HDR cube build target or color space is invalid."});
			}
			auto PlatformData = std::make_unique<FTextureCubePlatformData>();
			if (auto Result = TextureCubeBuilder::BuildHDRTextureCube(*Request.HDRPanorama,
				Request.PanoramaSettings, *PlatformData); !Result) return std::unexpected(std::move(Result.error()));
			return PlatformData;
		}
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| !Request.DecodedFaces.get().IsValid())
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build,
				"TextureCube canonical build request is invalid."});
		}
		const FTextureCubeDecodedFaces& SourceData = Request.DecodedFaces.get();
		const bool bHasTransparency = SourceData.TransparencyMask != 0;
		auto PlatformData = std::make_unique<FTextureCubePlatformData>();
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const std::expected<void, FTexture2DBuildError> BuildResult = TextureBuilder::BuildMipChain(
				std::span(&SourceData.Faces[Index], 1), ETextureUsage::Color,
				Request.bSRGB, PlatformData->Faces[Index], 0,
				ETextureCompressionQuality::Normal, ETextureAlphaMipMode::Average,
				0.5f, nullptr, bHasTransparency);
			if (!BuildResult)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build,
					std::format("{} face platform build failed: {}",
					FaceNames[Index], Durin::FormatTexture2DBuildError(BuildResult.error()))});
			}
		}
		PlatformData->PixelFormat = PlatformData->Faces[0].PixelFormat;
		if (!PlatformData->IsValid())
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build,
				"Cube texture platform data is inconsistent."});
		}
		return PlatformData;
	}
}
