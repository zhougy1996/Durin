#pragma once

#include "RHIDefinitions.h"

namespace Durin
{
	// Collects, validates, and imports the six oriented sources of a cube texture.
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
		auto BrowseDestination() -> void;
		auto RevalidateSources() -> bool;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;
		std::string PreferredDestinationDirectory;
		std::array<std::array<char, 512>, TextureCubeFaceCount> FacePathBuffers{};
		std::array<char, 256> AssetPathBuffer{};
		std::string SourceValidationMessage;
		uint32 ValidatedDimension = 0;
		uint32 ValidatedMipCount = 0;
		EPixelFormat ValidatedPixelFormat = EPixelFormat::Unknown;
		bool bSourcesValid = false;
		bool bOpenRequested = false;
	};
}
