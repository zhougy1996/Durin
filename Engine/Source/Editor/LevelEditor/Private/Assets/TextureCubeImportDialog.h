#pragma once

#include "Assets/MountedSourceImport.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	// Collects, validates, and imports either six oriented faces or one equirectangular panorama.
	class FTextureCubeImportDialog
	{
	public:
		FTextureCubeImportDialog(std::function<void()> InClearError,
			std::function<void(std::string)> InReportError,
			std::function<void(std::string)> InImported = {});

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

		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;
		std::string PreferredDestinationDirectory;
		std::array<std::array<char, 512>, TextureCubeFaceCount> FacePathBuffers{};
		std::array<std::array<char, 512>, TextureCubeFaceCount> FaceDestinationBuffers{};
		std::array<char, 512> PanoramaPathBuffer{};
		std::array<char, 512> PanoramaDestinationBuffer{};
		std::array<char, 256> AssetPathBuffer{};
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
		bool bOpenRequested = false;
	};
}
