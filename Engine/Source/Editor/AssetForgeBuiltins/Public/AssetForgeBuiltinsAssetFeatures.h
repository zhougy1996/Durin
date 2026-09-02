#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetSaveReadiness.h"

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
