#include "BuiltinImportProviderCommon.h"
#include "Texture2DPostLoad.h"
#include "TextureCubePostLoadPolicy.h"

#include "DObject/Package.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "Terrain/TerrainHeightmap.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Texture/VolumeTextureBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	auto FAssetForgeBuiltinsAssetFeatures::Validate(const DObject& Object) const
		-> FAssetSaveReadinessFeatureResult
	{
		auto NotReady = [](std::string_view Domain) {
			return FAssetSaveReadinessFeatureResult{
				.bHandled = true,
				.Result = {Asset::EAssetError::StaleData,
					std::format("{} post-load recovery did not publish domain-ready data.", Domain)}};
		};
		if (const auto* Mesh = Cast<DStaticMesh>(&Object))
			return Mesh->GetRenderData()
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("StaticMesh");
		if (const auto* Mesh = Cast<DSkeletalMesh>(&Object))
			return Mesh->GetRenderData()
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("SkeletalMesh");
		if (const auto* Texture = Cast<DTexture2D>(&Object))
			return Texture->GetPlatformData()
				&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("Texture2D");
		if (const auto* Texture = Cast<DTextureCube>(&Object))
			return Texture->GetPlatformData()
				&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("TextureCube");
		if (const auto* Texture = Cast<DVolumeTexture>(&Object))
			return Texture->GetPlatformData()
				&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("VolumeTexture");
		if (const auto* Heightmap = Cast<DTerrainHeightmap>(&Object))
			return Heightmap->GetPayload()
				&& Heightmap->GetStatus() == ETerrainHeightmapStatus::Ready
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("TerrainHeightmap");
		return {};
	}

	auto FAssetForgeBuiltinsAssetFeatures::BuildFileProduct(
		DStaticMesh& Mesh,
		std::string_view SourcePath,
		FStaticMeshSourceImportData SourceImportData,
		std::string_view SourceContentHash,
		FStaticMeshBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		return BuildStaticMeshFileProduct(
			Mesh, SourcePath, std::move(SourceImportData), SourceContentHash, OutProduct, OutError);
	}

	auto FAssetForgeBuiltinsAssetFeatures::PostLoadUncooked(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		return PostLoadStaticMesh(Mesh, OutDiagnostic, OutError);
	}

	auto FAssetForgeBuiltinsAssetFeatures::PostLoadUncooked(
		DTexture2D& Texture, std::string& OutError) -> bool
	{
		return PostLoadTexture2DFeature(Texture, OutError);
	}

	auto FAssetForgeBuiltinsAssetFeatures::PostLoadUncooked(
		DTextureCube& Texture, std::string& OutError) -> bool
	{
		return PostLoadTextureCubeFeature(Texture, OutError);
	}

}
