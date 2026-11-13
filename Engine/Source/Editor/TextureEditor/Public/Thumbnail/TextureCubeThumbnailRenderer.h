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
	class FTextureCubeThumbnailGenerationInput final
		: public IAssetThumbnailGenerationInput
	{
	public:
		explicit FTextureCubeThumbnailGenerationInput(FTopLevelAssetPath InAssetPath)
			: AssetPath(std::move(InAssetPath))
		{
		}

		FTopLevelAssetPath AssetPath;
	};

	// Captures the exact TextureCube package identity and preview visual contract.
	DCLASS()
	class DTextureCubeThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		TEXTUREEDITOR_API explicit DTextureCubeThumbnailRenderer(const FObjectInitializer& Initializer);
		TEXTUREEDITOR_API auto GetRegistration() const
			-> FThumbnailRenderingInfo override;
		TEXTUREEDITOR_API auto CaptureGenerationInput(const FAssetThumbnailPackageFingerprint& Asset)
			-> std::expected<FThumbnailGenerationData, std::string> override;
		TEXTUREEDITOR_API auto CreateGenerationSession(const IAssetThumbnailGenerationInput& Input)
			-> std::unique_ptr<IThumbnailRendererSession> override;
	};
} // namespace Durin::TextureEditor
