#include "AssetForgeBuiltinsAssetFeatures.h"

#include "DObject/Package.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace Durin::AssetForge::Builtins
{
	auto FAssetForgeBuiltinsAssetFeatures::Validate(const DObject& Object) const
		-> FAssetSaveReadinessFeatureResult
	{
		auto NotReady = [](std::string_view Domain) {
			return FAssetSaveReadinessFeatureResult{
				.bHandled = true,
				.Result = {EAssetError::StaleData,
					std::format("{} post-load recovery did not publish domain-ready data.", Domain)}};
		};
		if (const auto* Mesh = Cast<DStaticMesh>(&Object))
			return Mesh->GetRenderData()
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("StaticMesh");
		if (const auto* Texture = Cast<DTexture2D>(&Object))
			return Texture->GetPlatformData()
				&& Texture->HasPlatformData()
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("Texture2D");
		if (const auto* Texture = Cast<DTextureCube>(&Object))
			return Texture->GetPlatformData()
				&& Texture->HasPlatformData()
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("TextureCube");
		if (const auto* Texture = Cast<DVolumeTexture>(&Object))
			return Texture->GetPlatformData()
				&& Texture->HasPlatformData()
				? FAssetSaveReadinessFeatureResult{.bHandled = true}
				: NotReady("VolumeTexture");
		return {};
	}

}
