#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetSaveReadiness.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin::AssetForge::Builtins
{
	class FAssetForgeBuiltinsAssetFeatures final
		: public IAssetSaveReadinessFeature
	{
	public:
		ASSETFORGEBUILTINS_API auto Validate(const DObject& Asset) const
			-> FAssetSaveReadinessFeatureResult override;
	};
}
