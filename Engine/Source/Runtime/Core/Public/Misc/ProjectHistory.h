#pragma once

#include "CoreAPI.h"
#include "Misc/ProjectError.h"

namespace Durin
{
	// Describes whether a persisted recent-project entry can currently be opened.
	enum class ERecentProjectStatus : uint8
	{
		Available,
		Missing,
		Invalid,
	};

	// Stores one normalized recent-project entry and its latest validation result.
	struct FRecentProjectInfo
	{
		std::string Name;
		std::string ProjectFile;
		ERecentProjectStatus Status = ERecentProjectStatus::Missing;
		std::string Error;
	};

	// Persists a bounded, most-recent-first project list and refreshes entry validity.
	class FProjectHistory
	{
	public:
		static constexpr size_t MaximumRecentProjects = 10;

		CORE_API explicit FProjectHistory(std::string HistoryFile);

		[[nodiscard]] CORE_API auto Load() -> std::expected<void, FProjectError>;
		[[nodiscard]] CORE_API auto Record(std::string_view ProjectName, std::string_view ProjectFile) -> std::expected<void, FProjectError>;
		[[nodiscard]] CORE_API auto Remove(std::string_view ProjectFile) -> std::expected<void, FProjectError>;

		auto GetEntries() const -> const std::vector<FRecentProjectInfo>& { return Entries; }
		auto GetMostRecentProjectFile() const -> std::string { return Entries.empty() ? std::string{} : Entries.front().ProjectFile; }

	private:
		auto Save() const -> std::expected<void, FProjectError>;
		auto RefreshStatuses() -> void;

		std::string HistoryFile;
		std::vector<FRecentProjectInfo> Entries;
	};

	CORE_API auto MakeDefaultProjectHistory() -> FProjectHistory;
}
