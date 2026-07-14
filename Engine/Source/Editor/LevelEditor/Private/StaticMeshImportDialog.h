#pragma once

namespace Durin
{
	class FStaticMeshImportDialog
	{
	public:
		FStaticMeshImportDialog(std::function<void()> InClearError, std::function<void(std::string)> InReportError);

		auto Open() -> void;
		auto Draw() -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestination() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::array<char, 512> SourcePathBuffer{};
		std::array<char, 256> AssetPathBuffer{};
		std::string LastSuggestedAssetPath;
		bool bOpenRequested = false;
	};
} // namespace Durin
