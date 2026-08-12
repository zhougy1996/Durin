#include "EngineAssetServices.h"

#include "AssetSystem.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	auto InitializeEngineAssetServices() -> void
	{
		static const bool bInitialized = [] {
			auto PreserveMountedSource = [](
				const Asset::FAssetData&,
				const Asset::FAssetPackageInspection&,
				Asset::FAssetDeleteContribution&
			) -> Asset::FAssetResult {
				// Mounted sources may be shared and require a separate, explicit source operation.
				return {};
			};
			Asset::RegisterAssetDeleteContributor(DTexture2D::StaticClass(), PreserveMountedSource);
			Asset::RegisterAssetDeleteContributor(DTextureCube::StaticClass(), PreserveMountedSource);
			Asset::RegisterAssetDeleteContributor(DStaticMesh::StaticClass(), PreserveMountedSource);
			Asset::RegisterAssetDeleteContributor(DTerrainHeightmap::StaticClass(), PreserveMountedSource);
			return true;
		}();
		(void)bInitialized;
	}
}
