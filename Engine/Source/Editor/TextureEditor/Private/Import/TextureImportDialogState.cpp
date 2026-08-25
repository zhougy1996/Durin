#include "Import/TextureImportDialogState.h"

#include "AssetForge/Builtins/VolumeTextureImport.h"
#include "Texture/Texture2D.h"

namespace Durin::Editor::Texture
{
	auto FTexture2DImportFormState::Reset() -> void
	{
		Source.Reset();
		Usage = ETextureUsage::Color;
	}

	auto FVolumeTextureImportFormState::Reset() -> void
	{
		Source.Reset();
		Channels = EVolumeTextureSourceChannels::Red;
		SliceWidth = 128;
		SliceHeight = 128;
		Depth = 128;
		TilesX = 12;
		TilesY = 12;
	}

	auto FTextureCubeImportFormState::Reset() -> void
	{
		for (auto& Buffer : FacePathBuffers) Buffer.fill(0);
		for (auto& Buffer : FaceDestinationBuffers) Buffer.fill(0);
		for (std::string& Path : LastSuggestedFaceDestinations) Path.clear();
		PanoramaPathBuffer.fill(0);
		PanoramaDestinationBuffer.fill(0);
		LastSuggestedPanoramaDestination.clear();
		SourceValidationMessage = "Select a 2:1 panorama to continue.";
		SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		PanoramaFaceDimension = 0;
		PanoramaCustomFaceDimension = 0;
		PanoramaExposureEV = 0.0f;
		ValidatedSourceWidth = 0;
		ValidatedSourceHeight = 0;
		ValidatedDimension = 0;
		ValidatedMipCount = 0;
		ValidatedPixelFormat = EPixelFormat::Unknown;
		bValidatedHDR = false;
		bSourcesValid = false;
	}

	auto FTextureImportDialogState::Reset() -> void
	{
		AssetType = ETextureImportAssetType::Texture2D;
		SourceMode = EMountedSourceImportMode::IngestExternal;
		Texture2D.Reset();
		TextureCube.Reset();
		VolumeTexture.Reset();
	}
} // namespace Durin::Editor::Texture
