#pragma once

#include "Thumbnail/RenderedAssetThumbnailExtension.h"
#include "TextureEditorAPI.h"

namespace Durin
{
	// Immutable provider input used by the shared rendered-thumbnail scheduler.
	class FTextureCubeThumbnailGenerationInput final
		: public Editor::IAssetThumbnailGenerationInput
	{
	public:
		explicit FTextureCubeThumbnailGenerationInput(FAssetPath InAssetPath)
			: AssetPath(std::move(InAssetPath))
		{
		}

		FAssetPath AssetPath;
	};

	// Captures the exact TextureCube package identity and preview visual contract.
	class FTextureCubeAssetThumbnailProvider final : public Editor::IRenderedAssetThumbnailExtension
	{
	public:
		TEXTUREEDITOR_API auto GetRegistration() const
			-> Editor::FAssetThumbnailProviderRegistration override;
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		TEXTUREEDITOR_API auto CreateGenerationSession(
			const Editor::FAssetThumbnailGenerationRequest& Request,
			const Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<Editor::IRenderedAssetThumbnailGenerationSession> override;
	};
} // namespace Durin
