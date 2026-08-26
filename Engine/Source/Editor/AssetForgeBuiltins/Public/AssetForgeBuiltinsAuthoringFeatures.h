#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetAuthoringReadiness.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureCubePostLoad.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin::AssetForge::Builtins
{
	class FAssetForgeBuiltinsAuthoringFeatures final
		: public IStaticMeshAuthoringFeature
		, public ITexture2DPostLoadFeature
		, public ITexture2DImportRecoveryFeature
		, public ITextureCubeAuthoringFeature
		, public IVolumeTextureImportRecoveryFeature
		, public IAssetAuthoringReadinessFeature
	{
	public:
		ASSETFORGEBUILTINS_API auto Validate(const DObject& Asset) const
			-> FAssetAuthoringReadinessFeatureResult override;
		ASSETFORGEBUILTINS_API auto BuildFileProduct(
			DStaticMesh& Mesh,
			std::string_view SourcePath,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceContentHash,
			FStaticMeshAuthoringProduct& OutProduct,
			std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto ChangeSourceReference(
			DStaticMesh& Mesh,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DTexture2D& Texture, std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto WaitForRecovery(
			DTexture2D& Texture, double TimeoutSeconds) -> bool override;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DTextureCube& Texture, std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto RecoverUncooked(
			DVolumeTexture& Texture, std::string& OutError) -> bool override;
	};
}
