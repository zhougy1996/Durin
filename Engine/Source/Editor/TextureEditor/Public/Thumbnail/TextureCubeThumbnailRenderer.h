#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/ThumbnailRenderer.h"
#include "TextureEditorAPI.h"

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
	class DTextureCubeThumbnailRenderer final : public DThumbnailRenderer
	{
	public:
		TEXTUREEDITOR_API auto GetRegistration() const
			-> FThumbnailRenderingInfo override;
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 RendererGeneration)
			-> std::expected<FAssetThumbnailGenerationRequest, std::string> override;
		TEXTUREEDITOR_API auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input)
			-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string> override;
	};
} // namespace Durin::TextureEditor
