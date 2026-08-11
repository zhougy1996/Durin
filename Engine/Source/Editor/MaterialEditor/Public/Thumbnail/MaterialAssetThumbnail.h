#pragma once

#include "MaterialEditorAPI.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin { class DMaterialInterface; }

namespace Durin::Editor::Material
{
	// Captures deterministic material dependency keys for one exact material asset class.
	class FMaterialAssetThumbnailProvider final : public ::Durin::Editor::IRenderedAssetThumbnailExtension
	{
	public:
		MATERIALEDITOR_API explicit FMaterialAssetThumbnailProvider(std::string AssetClassName);

		MATERIALEDITOR_API auto GetRegistration() const -> ::Durin::Editor::FAssetThumbnailProviderRegistration override;
		MATERIALEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
		MATERIALEDITOR_API auto CreateGenerationSession(
			const ::Durin::Editor::FAssetThumbnailGenerationRequest& Request,
			const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<::Durin::Editor::IRenderedAssetThumbnailGenerationSession> override;

	private:
		std::string AssetClassName;
	};
} // namespace Durin::Editor::Material
