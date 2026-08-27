#include "TextureCubePostLoadPolicy.h"

#include "Texture/TextureCube.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/TextureCubePostLoad.h"
#include "AssetForge/Builtins/TextureCubeImport.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		auto PostLoadTextureCubeImpl(DTextureCube& Texture, std::string& OutError) -> bool
		{
			std::string Key = Asset::MakeTextureCubeDerivedDataKey(Texture, OutError);
			if (!Key.empty())
			{
				std::unique_ptr<FTextureCubePlatformData> PlatformData;
				ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
				std::string Message;
				if (Asset::LoadTextureCubeDerivedData(Key, PlatformData, Status, Message))
					return Texture.PublishDerivedDataLoad(
						std::move(PlatformData), std::move(Key), OutError);
			}

			return RecoverTextureCubeDerivedData(Texture, OutError);
		}
	}

	auto PostLoadTextureCubeFeature(DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeImpl(Texture, OutError);
	}
}
