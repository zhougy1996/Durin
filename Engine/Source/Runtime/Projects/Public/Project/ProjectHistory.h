#pragma once

#include "CoreMinimal.h"

#include "ProjectsAPI.h"
#include "Project/ProjectError.h"

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

		PROJECTS_API explicit FProjectHistory(std::string HistoryFile);

		[[nodiscard]] PROJECTS_API auto Load() -> std::expected<void, FProjectError>;
		[[nodiscard]] PROJECTS_API auto Record(std::string_view ProjectName, std::string_view ProjectFile, bool bUpdateRestoreTarget = true) -> std::expected<void, FProjectError>;
		[[nodiscard]] PROJECTS_API auto Remove(std::string_view ProjectFile) -> std::expected<void, FProjectError>;

		auto GetEntries() const -> const std::vector<FRecentProjectInfo>& { return Entries; }
		auto GetMostRecentProjectFile() const -> std::string { return Entries.empty() ? std::string{} : Entries.front().ProjectFile; }
		auto GetRestoreProjectFile() const -> std::string { return bAutoRestoreEnabled ? RestoreProjectFile : std::string{}; }
		[[nodiscard]] PROJECTS_API auto SetAutoRestoreEnabled(bool bEnabled) -> std::expected<void, FProjectError>;

	private:
		auto Save() const -> std::expected<void, FProjectError>;
		auto RefreshStatuses() -> void;

		std::string HistoryFile;
		std::vector<FRecentProjectInfo> Entries;
		std::string RestoreProjectFile;
		bool bAutoRestoreEnabled = true;
	};

	PROJECTS_API auto MakeDefaultProjectHistory() -> FProjectHistory;
}
