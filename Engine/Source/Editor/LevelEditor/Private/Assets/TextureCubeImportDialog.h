#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	// Collects, validates, and imports either six oriented faces or one equirectangular panorama.
	class FTextureCubeImportDialog
	{
	public:
		explicit FTextureCubeImportDialog(FImportDialogCallbacks InCallbacks);

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto BrowseFace(ETextureCubeFace Face) -> void;
		auto BrowsePanorama() -> void;
		auto BrowseDestination() -> void;
		auto RevalidateSources() -> bool;
		auto Import() -> bool;
		auto SuggestAssetPath(std::string_view SourceFile) -> void;
		auto SuggestSourceDestinations() -> void;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		std::array<std::array<char, 512>, TextureCubeFaceCount> FacePathBuffers{};
		std::array<std::array<char, 512>, TextureCubeFaceCount> FaceDestinationBuffers{};
		std::array<char, 512> PanoramaPathBuffer{};
		std::array<char, 512> PanoramaDestinationBuffer{};
		std::string SourceValidationMessage;
		ETextureCubeSourceLayout SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		uint32 PanoramaFaceDimension = 0;
		uint32 PanoramaCustomFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		uint32 ValidatedSourceWidth = 0;
		uint32 ValidatedSourceHeight = 0;
		uint32 ValidatedDimension = 0;
		uint32 ValidatedMipCount = 0;
		EPixelFormat ValidatedPixelFormat = EPixelFormat::Unknown;
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
		bool bValidatedHDR = false;
		bool bSourcesValid = false;
	};
}
