#include "TextureCubePostLoadPolicy.h"

#include "EncodedSourceSnapshot.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubePostLoad.h"
#include "TextureCubeBuildAdapter.h"
#include "TextureCubeSourceTranslation.h"
#include "ImportService.h"

namespace Durin::Asset::Forge
{
	namespace
	{
		auto PostLoadTextureCubeImpl(DTextureCube& Texture, std::string& OutError) -> bool
		{
			std::string Key = Asset::Build::MakeTextureCubeDerivedDataKey(Texture, OutError);
			if (!Key.empty())
			{
				std::unique_ptr<FTextureCubePlatformData> PlatformData;
				ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
				std::string Message;
				if (Asset::Build::LoadTextureCubeDerivedData(Key, PlatformData, Status, Message))
					return Texture.PublishDerivedDataLoad(
						std::move(PlatformData), std::move(Key), OutError);
			}

			const FTextureCubeSourceImportData& Source = Texture.GetSourceImportData();
			std::array<FSourcePath, TextureCubeFaceCount> Paths;
			const size_t SourceCount = Source.SourceLayout
				== ETextureCubeSourceLayout::SixFaces ? TextureCubeFaceCount : 1;
			if (Source.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
				Paths[0] = Source.Panorama.SourcePath;
			else for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceFile& Face = Source.GetFace(static_cast<ETextureCubeFace>(Index));
				Paths[Index] = Face.SourcePath;
			}
			FAssetPath Destination;
			if (!Texture.GetPackage()
				|| !FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError))
				return false;
			FInterchangeProvenance Existing;
			std::optional<FInterchangeProvenance> Provenance;
			if (InspectTextureCubeInterchangeProvenance(Texture, Existing, OutError))
				Provenance = std::move(Existing);
			else OutError.clear();
			FInterchangeImportRequest Request;
			if (!MakeTextureCubeInterchangeRequest(std::span(Paths).first(SourceCount),
				Source.SourceLayout, Destination, {.bSRGB = Texture.IsSRGB()},
				{.FaceDimension = Texture.GetPanoramaFaceDimension(),
					.ExposureEV = Texture.GetPanoramaExposureEV()},
				EInterchangeImportMode::Recover,
				{.OwnerId = std::format("TextureCube.Recovery:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(Provenance), Request, OutError)) return false;
			Request.Lifetime = EImportOperationLifetime::SessionCritical;
			const FInterchangeImportHandle Handle = GetImportService().SubmitInterchangeImport(
				std::move(Request), std::format("Recover TextureCube {}", Destination.GetAssetName()));
			if (!Handle)
			{
				OutError = "TextureCube Interchange recovery could not be submitted.";
				return false;
			}
			OutError.clear();
			return true;
		}
	}

	auto PostLoadTextureCubeFeature(DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeImpl(Texture, OutError);
	}
}
