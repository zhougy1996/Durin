#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetSaveReadiness.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshPostLoad.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin::AssetForge::Builtins
{
	class FAssetForgeBuiltinsAssetFeatures final
		: public IStaticMeshBuildFeature
		, public IStaticMeshPostLoadFeature
		, public ITexture2DPostLoadFeature
		, public ITextureCubePostLoadFeature
		, public IAssetSaveReadinessFeature
	{
	public:
		ASSETFORGEBUILTINS_API auto Validate(const DObject& Asset) const
			-> FAssetSaveReadinessFeatureResult override;
		ASSETFORGEBUILTINS_API auto BuildFileProduct(
			DStaticMesh& Mesh,
			std::string_view SourcePath,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceContentHash,
			FStaticMeshBuildProduct& OutProduct,
			std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DTexture2D& Texture, std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DTextureCube& Texture, std::string& OutError) -> bool override;
	};
}
