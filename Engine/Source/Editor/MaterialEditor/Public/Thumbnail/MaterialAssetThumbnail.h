#pragma once

#include "MaterialEditorAPI.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin
{
	class DMaterialInterface;

	// Captures deterministic material dependency keys for one exact material asset class.
	class FMaterialAssetThumbnailProvider final : public Editor::IRenderedAssetThumbnailExtension
	{
	public:
		MATERIALEDITOR_API explicit FMaterialAssetThumbnailProvider(std::string AssetClassName);

		MATERIALEDITOR_API auto GetRegistration() const -> Editor::FAssetThumbnailProviderRegistration override;
		MATERIALEDITOR_API auto CaptureGenerationRequest(
			const Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		MATERIALEDITOR_API auto CreateGenerationSession(
			const Editor::FAssetThumbnailGenerationRequest& Request,
			const Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<Editor::IRenderedAssetThumbnailGenerationSession> override;

	private:
		std::string AssetClassName;
	};
} // namespace Durin
