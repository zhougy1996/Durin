#pragma once

#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	enum class EStaticMeshImportPreset : uint8
	{
		Durin,
		YUpNegativeZForward,
		Custom
	};

	class FStaticMeshImportDialog
	{
	public:
		FStaticMeshImportDialog(std::function<void()> InClearError, std::function<void(std::string)> InReportError, std::function<void(std::string)> InImported = {});

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestination() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;
		std::string PreferredDestinationDirectory;
		std::array<char, 512> SourcePathBuffer{};
		std::array<char, 256> AssetPathBuffer{};
		std::string LastSuggestedAssetPath;
		FStaticMeshImportSettings ImportSettings;
		EStaticMeshImportPreset ImportPreset = EStaticMeshImportPreset::Durin;
		bool bOpenRequested = false;
	};
} // namespace Durin
