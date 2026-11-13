#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/ThumbnailRenderer.h"
#include "TextureEditorAPI.h"

#include "TextureThumbnailRenderer.gen.h"

namespace Durin::TextureEditor
{
	// Captures built GPU texture output through the shared thumbnail scheduler.
	DCLASS()
	class DTextureThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		TEXTUREEDITOR_API explicit DTextureThumbnailRenderer(const FObjectInitializer& Initializer);
		TEXTUREEDITOR_API auto GetRegistration() const
			-> FThumbnailRenderingInfo override;
		TEXTUREEDITOR_API auto DescribeThumbnail(const FAssetThumbnailPackageFingerprint& Asset)
			-> std::expected<FThumbnailCacheDescription, std::string> override;
		TEXTUREEDITOR_API auto CanVisualizeAsset(DObject* Object) const -> bool override;
		TEXTUREEDITOR_API auto Draw(DObject* Object, FThumbnailDrawContext& Context) -> void override;
	};
} // namespace Durin::TextureEditor
