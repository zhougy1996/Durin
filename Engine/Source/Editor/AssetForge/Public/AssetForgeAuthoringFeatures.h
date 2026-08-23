#pragma once

#include "AssetForgeAPI.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin::Asset::Forge
{
	class FAssetForgeAuthoringFeatures final
		: public IStaticMeshAuthoringFeature
		, public ITexture2DAuthoringFeature
		, public ITexture2DInterchangeRecoveryFeature
		, public ITextureCubeAuthoringFeature
		, public IVolumeTextureInterchangeRecoveryFeature
	{
	public:
		ASSETFORGE_API auto BuildFileProduct(
			DStaticMesh& Mesh,
			std::string_view SourcePath,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceContentHash,
			FStaticMeshAuthoringProduct& OutProduct,
			std::string& OutError) -> bool override;
		ASSETFORGE_API auto PostLoadUncooked(
			DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool override;
		ASSETFORGE_API auto ChangeSourceReference(
			DStaticMesh& Mesh,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool override;
		ASSETFORGE_API auto PostLoadUncooked(
			DTexture2D& Texture, std::string& OutError) -> bool override;
		ASSETFORGE_API auto WaitForRecovery(
			DTexture2D& Texture, double TimeoutSeconds) -> bool override;
		ASSETFORGE_API auto PostLoadUncooked(
			DTextureCube& Texture, std::string& OutError) -> bool override;
		ASSETFORGE_API auto RecoverUncooked(
			DVolumeTexture& Texture, std::string& OutError) -> bool override;
	};
}
