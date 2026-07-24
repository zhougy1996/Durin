#pragma once

#include "CoreAPI.h"

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

		CORE_API FProjectHistory(std::string HistoryFile, std::string LegacySessionFile = {});

		CORE_API auto Load(std::string* OutError = nullptr) -> bool;
		CORE_API auto Record(std::string_view ProjectName, std::string_view ProjectFile, std::string* OutError = nullptr) -> bool;
		CORE_API auto Remove(std::string_view ProjectFile, std::string* OutError = nullptr) -> bool;

		auto GetEntries() const -> const std::vector<FRecentProjectInfo>& { return Entries; }
		auto GetMostRecentProjectFile() const -> std::string { return Entries.empty() ? std::string{} : Entries.front().ProjectFile; }

	private:
		auto Save(std::string* OutError) const -> bool;
		auto RefreshStatuses() -> void;

		std::string HistoryFile;
		std::string LegacySessionFile;
		std::vector<FRecentProjectInfo> Entries;
	};

	CORE_API auto MakeDefaultProjectHistory() -> FProjectHistory;
}
