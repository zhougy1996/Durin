#include "Misc/ProjectHistory.h"

#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringHelper.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	namespace
	{
		constexpr const char* ProjectHistoryFileName = "ProjectHistory.yaml";
		auto MakeProjectKey(std::string_view ProjectFile) -> std::string
		{
			std::string Key = NormalizeProjectFile(ProjectFile);
#if PLATFORM_WINDOWS
			Key = StringUtils::FoldAscii(Key);
#endif
			return Key;
		}

		auto InspectProject(FRecentProjectInfo& Entry) -> void
		{
			Entry.Error.clear();
			if (!std::filesystem::is_regular_file(Entry.ProjectFile))
			{
				Entry.Status = ERecentProjectStatus::Missing;
				Entry.Error = "Project file does not exist.";
				return;
			}

			FJsonDocument Descriptor;
			if (const auto Loaded = Descriptor.LoadFromFile(Entry.ProjectFile); !Loaded)
			{
				Entry.Status = ERecentProjectStatus::Invalid;
				Entry.Error = Loaded.error().ToString();
				return;
			}

			const std::string ProjectName = Descriptor.GetRootView().GetView("ProjectName").GetString();
			if (ProjectName.empty())
			{
				Entry.Status = ERecentProjectStatus::Invalid;
				Entry.Error = "Project descriptor has no ProjectName.";
				return;
			}

			Entry.Name = ProjectName;
			Entry.Status = ERecentProjectStatus::Available;
		}

	}

	FProjectHistory::FProjectHistory(std::string InHistoryFile)
		: HistoryFile(NormalizeProjectFile(InHistoryFile))
	{
	}

	auto FProjectHistory::Load() -> std::expected<void, FProjectError>
	{
		Entries.clear();
		const bool bHistoryExists = std::filesystem::exists(HistoryFile);
		if (bHistoryExists)
		{
			FYamlDocument Document;
			if (const auto Loaded = Document.LoadFromFile(HistoryFile); !Loaded)
			{
				return std::unexpected(FProjectError{EProjectError::HistoryLoad, std::format("Could not load project history '{}': {}", HistoryFile, Loaded.error().ToString())});
			}

			const FYamlNodeView RecentProjects = Document.GetRootView().GetView("RecentProjects");
			std::unordered_set<std::string> SeenProjects;
			for (size_t Index = 0; Index < RecentProjects.Num() && Entries.size() < MaximumRecentProjects; ++Index)
			{
				const FYamlNodeView Item = RecentProjects.GetView(Index);
				std::string ProjectFile = Item.GetView("ProjectFile").GetString();
				if (ProjectFile.empty()) continue;
				ProjectFile = NormalizeProjectFile(ProjectFile);
				if (!SeenProjects.insert(MakeProjectKey(ProjectFile)).second) continue;
				Entries.push_back({Item.GetView("Name").GetString(std::filesystem::path(ProjectFile).stem().string()), std::move(ProjectFile)});
			}
		}
		else
		{
			RefreshStatuses();
			return Save();
		}

		RefreshStatuses();
		return {};
	}

	auto FProjectHistory::Record(std::string_view ProjectName, std::string_view ProjectFile) -> std::expected<void, FProjectError>
	{
		const std::string Normalized = NormalizeProjectFile(ProjectFile);
		const std::string Key = MakeProjectKey(Normalized);
		std::erase_if(Entries, [&Key](const FRecentProjectInfo& Entry) { return MakeProjectKey(Entry.ProjectFile) == Key; });
		Entries.insert(Entries.begin(), {std::string(ProjectName), Normalized, ERecentProjectStatus::Available, {}});
		if (Entries.size() > MaximumRecentProjects) Entries.resize(MaximumRecentProjects);
		return Save();
	}

	auto FProjectHistory::Remove(std::string_view ProjectFile) -> std::expected<void, FProjectError>
	{
		const std::string Key = MakeProjectKey(ProjectFile);
		std::erase_if(Entries, [&Key](const FRecentProjectInfo& Entry) { return MakeProjectKey(Entry.ProjectFile) == Key; });
		return Save();
	}

	auto FProjectHistory::Save() const -> std::expected<void, FProjectError>
	{
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		Root.SetChildValue("Version", 1);
		FYamlNodeRef RecentProjects = Root.AddSequence("RecentProjects");
		for (const FRecentProjectInfo& Entry : Entries)
		{
			FYamlNodeRef Item = RecentProjects.AppendMap();
			Item.SetChildValue("Name", Entry.Name);
			Item.SetChildValue("ProjectFile", Entry.ProjectFile);
		}
		if (Document.SaveToFile(HistoryFile)) return {};
		return std::unexpected(FProjectError{EProjectError::HistorySave, std::format("Could not save project history '{}'.", HistoryFile)});
	}

	auto FProjectHistory::RefreshStatuses() -> void
	{
		for (FRecentProjectInfo& Entry : Entries) InspectProject(Entry);
	}

	auto MakeDefaultProjectHistory() -> FProjectHistory
	{
		return FProjectHistory(FPaths::LaunchConfigsDir() + ProjectHistoryFileName);
	}
}
