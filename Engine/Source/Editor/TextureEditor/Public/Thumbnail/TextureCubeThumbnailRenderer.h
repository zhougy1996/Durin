#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/ThumbnailRenderer.h"
#include "TextureEditorAPI.h"

#include "TextureCubeThumbnailRenderer.gen.h"

namespace Durin::TextureEditor
{
	// Freezes TextureEditor-owned presentation values for the wide environment view.
	struct FTextureCubeThumbnailRendererVisualContract
	{
		static constexpr float VerticalFieldOfViewDegrees = 100.0f;
	};

	// Immutable renderer input used by the shared rendered-thumbnail scheduler.

	// Captures the exact TextureCube package identity and preview visual contract.
	DCLASS()
	class DTextureCubeThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		TEXTUREEDITOR_API explicit DTextureCubeThumbnailRenderer(const FObjectInitializer& Initializer);
		TEXTUREEDITOR_API auto GetRegistration() const
			-> FThumbnailRenderingInfo override;
		TEXTUREEDITOR_API auto DescribeThumbnail(const FAssetThumbnailPackageFingerprint& Asset)
			-> std::expected<FThumbnailCacheDescription, std::string> override;
		TEXTUREEDITOR_API auto CanVisualizeAsset(DObject* Object) const -> bool override;
		TEXTUREEDITOR_API auto Draw(DObject* Object, FThumbnailDrawContext& Context) -> void override;
	};
} // namespace Durin::TextureEditor
