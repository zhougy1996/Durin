#pragma once

#include "LevelEditorAPI.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/DefaultSizedThumbnailRenderer.h"

namespace Durin { struct FTerrainHeightmapPayload; }

namespace Durin::Editor::Level
{
	inline constexpr uint32 TerrainHeightmapThumbnailDimension = 256;

	// Generates the fixed top-left-oriented grayscale preview from canonical samples.
	LEVELEDITOR_API auto GenerateTerrainHeightmapThumbnailPixels(
		const FTerrainHeightmapPayload& Payload,
		std::vector<std::byte>& OutPixels,
		std::string& OutError) -> bool;

	// Captures a canonical heightmap payload without source-image or Renderer dependencies.
	class DTerrainHeightmapThumbnailRenderer final
		: public ::Durin::Editor::DDefaultSizedThumbnailRenderer
	{
	public:
		LEVELEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FThumbnailRenderingInfo override;
		LEVELEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 RendererGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
	};
}
