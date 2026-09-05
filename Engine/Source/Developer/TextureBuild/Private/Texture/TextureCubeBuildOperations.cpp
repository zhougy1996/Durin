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
			if (!Faces->ImportedData.IsValid())
			{
				OutError = "TextureCube canonical imported faces are invalid.";
				return false;
			}
			OutCanonicalInput = {.ImportedData = Faces->ImportedData,
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
		return std::visit([&](const auto& Image) {
			FTextureCubeSourceData SourceData;
			if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
				Image, {Panorama.Settings.FaceDimension, Panorama.Settings.ExposureEV},
				SourceData, OutError)) return false;
			FTextureCubeImportedData ImportedData;
			if (!ImportedData.SetSourceData(SourceData))
			{
				OutError = "TextureCube canonical imported faces are invalid.";
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
			OutCanonicalInput = {.ImportedData = std::move(ImportedData),
				.AuthoredPanorama = std::move(AuthoredPanorama),
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.OriginalSourceWidth = Image.Width,
				.OriginalSourceHeight = Image.Height,
				.PanoramaFaceDimension = Panorama.Settings.FaceDimension,
				.PanoramaExposureEV = Panorama.Settings.ExposureEV,
				.bSRGB = true};
			OutError.clear();
			return true;
		}, Panorama.Image);
	}

	auto BuildTextureCube(const FTextureCubeRecipeBuildRequest& Request,
		FTextureCubeRecipeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game
			|| !Request.ImportedData.get().IsValid())
		{
			OutError = "TextureCube canonical build request is invalid.";
			return false;
		}
		const FTextureCubeSourceData SourceData = Request.ImportedData.get().ToSourceData();
		const bool bHasTransparency = std::ranges::any_of(
			SourceData.Faces, [](const FTextureSourceData& Face) {
				return Face.bHasTransparency;
			});
		auto PlatformData = std::make_unique<FTextureCubePlatformData>();
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			FTextureSourceData BuildSource = SourceData.Faces[Index];
			BuildSource.bHasTransparency = bHasTransparency;
			const FTexture2DBuildResult BuildResult = TextureBuilder::BuildMipChain(
				BuildSource, ETextureUsage::Color,
				Request.bSRGB, PlatformData->Faces[Index]);
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
