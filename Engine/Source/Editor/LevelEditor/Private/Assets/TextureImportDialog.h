#pragma once

#include "Assets/MountedSourceImport.h"

namespace Durin
{
	enum class ETextureUsage : uint8;

	// Collects texture import options and submits the selected source file.
	class FTextureImportDialog
	{
	public:
		FTextureImportDialog(std::function<void()> InClearError, std::function<void(std::string)> InReportError, std::function<void(std::string)> InImported = {});

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestination() -> void;
		auto BrowseSourceDestination() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;
		std::string PreferredDestinationDirectory;
		std::array<char, 512> SourcePathBuffer{};
		std::array<char, 256> AssetPathBuffer{};
		std::array<char, 512> SourceDestinationBuffer{};
		std::string LastSuggestedAssetPath;
		std::string LastSuggestedSourceDestination;
		ETextureUsage Usage = static_cast<ETextureUsage>(0);
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
		bool bOpenRequested = false;
	};
} // namespace Durin
