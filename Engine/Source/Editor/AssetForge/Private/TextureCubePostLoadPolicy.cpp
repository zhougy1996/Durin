#include "TextureCubePostLoadPolicy.h"

#include "EncodedSourceSnapshot.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubePostLoad.h"
#include "TextureCubeBuildAdapter.h"
#include "TextureCubeSourceTranslation.h"

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
			if (Source.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			{
				FEncodedSourceSnapshot Snapshot;
				if (!CaptureEncodedSource(Source.Panorama.SourcePath, Snapshot, OutError)) return false;
				FTextureCubePanoramaSourceData Panorama;
				const FTextureCubePanoramaImportSettings Settings{
					Texture.GetPanoramaFaceDimension(), Texture.GetPanoramaExposureEV()};
				return TranslateTextureCubePanoramaSource(
					Snapshot.GetBytes(), Snapshot.PhysicalPath.extension().generic_string(),
					Panorama, OutError)
					&& BuildAndPublishTextureCubePanorama(Texture, std::move(Panorama),
						Snapshot.ContentHash, Snapshot.SourcePath, Settings, OutError);
			}

			FTextureCubeSourceData SourceData;
			std::array<FEncodedSourceSnapshot, TextureCubeFaceCount> Snapshots;
			std::array<std::span<const std::byte>, TextureCubeFaceCount> EncodedFaces;
			std::array<FXxHash128, TextureCubeFaceCount> Hashes;
			std::array<FSourcePath, TextureCubeFaceCount> Paths;
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceFile& Face = Source.GetFace(static_cast<ETextureCubeFace>(Index));
				if (!CaptureEncodedSource(Face.SourcePath, Snapshots[Index], OutError)) return false;
				EncodedFaces[Index] = Snapshots[Index].GetBytes();
				Hashes[Index] = Snapshots[Index].ContentHash;
				Paths[Index] = Snapshots[Index].SourcePath;
			}
			return TranslateTextureCubeFaceSources(EncodedFaces, SourceData, OutError)
				&& BuildAndPublishTextureCubeFaces(Texture, std::move(SourceData),
					Hashes, Paths, {.bSRGB = Texture.IsSRGB()}, OutError);
		}
	}

	auto PostLoadTextureCubeFeature(DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeImpl(Texture, OutError);
	}
}
