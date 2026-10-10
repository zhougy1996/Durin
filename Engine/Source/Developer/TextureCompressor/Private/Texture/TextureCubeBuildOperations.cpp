#include "Texture/TextureCubeBuildOperations.h"

#include "Logging/LogMacros.h"
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
		if ((Request.TargetPlatform != ECookTargetPlatform::Win64
				&& Request.TargetPlatform != ECookTargetPlatform::MacOS)
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"TextureCube build target is unsupported."});
		}
		if (const auto* Faces = std::get_if<FTextureCubeFacesBuildInput>(&Request.Input.get()))
		{
			if (!Faces->FaceImages.IsValid())
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
			CanonicalInput = {.Pixels = FTextureCubeLDRCanonicalInput{.FaceImages = Faces->FaceImages,
					.SourceLayout = Faces->SourceLayout, .bSRGB = Faces->Settings.bSRGB},
				.OriginalSourceWidth = Faces->OriginalSourceWidth,
				.OriginalSourceHeight = Faces->OriginalSourceHeight,
				.PanoramaFaceDimension = Faces->PanoramaFaceDimension,
				.PanoramaExposureEV = Faces->PanoramaExposureEV};
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
			FTextureCubeFaceImages SourceData;
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
			if (bHDR)
				CanonicalInput.Pixels = FTextureCubeHDRCanonicalInput{.AuthoredPanorama = std::move(AuthoredPanorama)};
			else
				CanonicalInput.Pixels = FTextureCubeLDRCanonicalInput{.FaceImages = std::move(SourceData),
					.AuthoredPanorama = std::move(AuthoredPanorama),
					.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama, .bSRGB = true};
			CanonicalInput.OriginalSourceWidth = Image.Width;
			CanonicalInput.OriginalSourceHeight = Image.Height;
			CanonicalInput.PanoramaFaceDimension = Panorama.Settings.FaceDimension;
			CanonicalInput.PanoramaExposureEV = Panorama.Settings.ExposureEV;
			return std::move(CanonicalInput);
		}, Panorama.Image);
	}

	auto BuildTextureCube(const FTextureCubeBuildInput& Request) -> std::optional<FTextureCubePlatformData>
	{
		auto Fail = [](std::string_view Reason) -> std::optional<FTextureCubePlatformData>
		{
			DURIN_ERROR_CATEGORY("TextureCompressor", "TextureCube build failed: {}", Reason);
			return std::nullopt;
		};
		if (const auto* HDR = std::get_if<FTextureCubeHDRBuildInput>(&Request.Pixels))
		{
			if ((Request.TargetPlatform != ECookTargetPlatform::Win64
					&& Request.TargetPlatform != ECookTargetPlatform::MacOS)
				|| Request.TargetProfile != ECookTargetProfile::Game)
				return Fail("HDR cube build target or color space is invalid.");
			FTextureCubePlatformData PlatformData;
			if (auto Result = TextureCubeBuilder::BuildHDRTextureCube(HDR->Panorama.get(),
				{.FaceDimension = HDR->FaceDimension, .ExposureEV = HDR->ExposureEV, .Output = ETextureCubeOutput::HDR}, PlatformData); !Result)
				return Fail(Result.error().Diagnostic);
			return PlatformData;
		}
		const auto& LDR = std::get<FTextureCubeLDRBuildInput>(Request.Pixels);
		if ((Request.TargetPlatform != ECookTargetPlatform::Win64
				&& Request.TargetPlatform != ECookTargetPlatform::MacOS)
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| !LDR.FaceImages.get().IsValid())
			return Fail("TextureCube canonical build request is invalid.");
		const FTextureCubeFaceImages& SourceData = LDR.FaceImages.get();
		const auto PixelFormat = TextureBuilder::SelectPixelFormat(ETextureUsage::Color, LDR.bSRGB, SourceData.TransparencyMask != 0);
		FTextureCubePlatformData PlatformData;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			auto BuildResult = TextureBuilder::BuildMipChain({
				.SourceMips = std::span(&SourceData.Faces[Index], 1),
				.Settings = {.bSRGB = LDR.bSRGB},
				.PixelFormat = PixelFormat});
			if (!BuildResult)
				return Fail(std::format("{} face platform build failed: {}", FaceNames[Index], BuildResult.error()));
			PlatformData.Faces[Index] = std::move(BuildResult->PlatformData);
		}
		PlatformData.PixelFormat = PlatformData.Faces[0].PixelFormat;
		if (!PlatformData.IsValid())
			return Fail("Cube texture platform data is inconsistent.");
		return PlatformData;
	}
}
