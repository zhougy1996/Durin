#pragma once

#include "LevelEditorAPI.h"
#include "Thumbnail/AssetThumbnailProvider.h"

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
	class FTerrainHeightmapAssetThumbnailProvider final
		: public ::Durin::Editor::IAssetThumbnailProvider
	{
	public:
		LEVELEDITOR_API auto GetRegistration() const
			-> ::Durin::Editor::FAssetThumbnailProviderRegistration override;
		LEVELEDITOR_API auto CaptureGenerationRequest(
			const ::Durin::Editor::FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override;
	};
}
