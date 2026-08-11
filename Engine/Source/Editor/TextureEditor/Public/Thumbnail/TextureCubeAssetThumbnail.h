#pragma once

#include "Thumbnail/RenderedAssetThumbnailExtension.h"
#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Immutable provider input used by the shared rendered-thumbnail scheduler.
	class FTextureCubeThumbnailGenerationInput final
		: public ::Durin::Editor::IAssetThumbnailGenerationInput
	{
	public:
		explicit FTextureCubeThumbnailGenerationInput(FAssetPath InAssetPath)
			: AssetPath(std::move(InAssetPath))
		{
		}

		FAssetPath AssetPath;
	};

	// Captures the exact TextureCube package identity and preview visual contract.
	class FTextureCubeAssetThumbnailProvider final : public ::Durin::Editor::IRenderedAssetThumbnailExtension
	{
	public:
		TEXTUREEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FAssetThumbnailProviderRegistration override;
		TEXTUREEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		TEXTUREEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<::Durin::Editor::IRenderedAssetThumbnailGenerationSession> override;
	};
} // namespace Durin::Editor::Texture
