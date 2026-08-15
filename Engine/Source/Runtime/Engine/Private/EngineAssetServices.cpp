#include "EngineAssetServices.h"

#include "AssetMutation.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	auto InitializeEngineAssetServices() -> void
	{
		static const std::array<Asset::FAssetDeleteContributorHandle, 4> Registrations = [] {
			auto PreserveMountedSource = [](
				const Asset::FAssetData&,
				const Asset::FAssetPackageInspection&,
				Asset::FAssetDeleteContribution&
			) -> Asset::FAssetResult {
				// Mounted sources may be shared and require a separate, explicit source operation.
				return {};
			};
			return std::array{
				Asset::RegisterAssetDeleteContributor(
					DTexture2D::StaticClass(), PreserveMountedSource),
				Asset::RegisterAssetDeleteContributor(
					DTextureCube::StaticClass(), PreserveMountedSource),
				Asset::RegisterAssetDeleteContributor(
					DStaticMesh::StaticClass(), PreserveMountedSource),
				Asset::RegisterAssetDeleteContributor(
					DTerrainHeightmap::StaticClass(), PreserveMountedSource),
			};
		}();
		(void)Registrations;
	}
}
