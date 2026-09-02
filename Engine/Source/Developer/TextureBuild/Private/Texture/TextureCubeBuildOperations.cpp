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
			OutCanonicalInput = {.ImportedData = std::move(ImportedData),
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
			if (!TextureBuilder::BuildMipChain(BuildSource, ETextureUsage::Color,
				Request.bSRGB, PlatformData->Faces[Index], OutError))
			{
				OutError = std::format("{} face platform build failed: {}",
					FaceNames[Index], OutError);
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
