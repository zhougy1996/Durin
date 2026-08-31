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
			FJsonParseError ParseError;
			if (!Descriptor.LoadFromFile(Entry.ProjectFile, &ParseError))
			{
				Entry.Status = ERecentProjectStatus::Invalid;
				Entry.Error = ParseError.Message;
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

		auto SetError(std::string* OutError, std::string Message) -> void
		{
			if (OutError) *OutError = std::move(Message);
		}
	}

	FProjectHistory::FProjectHistory(std::string InHistoryFile)
		: HistoryFile(NormalizeProjectFile(InHistoryFile))
	{
	}

	auto FProjectHistory::Load(std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		Entries.clear();
		const bool bHistoryExists = std::filesystem::exists(HistoryFile);
		if (bHistoryExists)
		{
			FYamlDocument Document;
			FYamlParseError ParseError;
			if (!Document.LoadFromFile(HistoryFile, &ParseError))
			{
				SetError(OutError, std::format("Could not load project history '{}': {}", HistoryFile, ParseError.Message));
				return false;
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
			return Save(OutError);
		}

		RefreshStatuses();
		return true;
	}

	auto FProjectHistory::Record(std::string_view ProjectName, std::string_view ProjectFile, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		const std::string Normalized = NormalizeProjectFile(ProjectFile);
		const std::string Key = MakeProjectKey(Normalized);
		std::erase_if(Entries, [&Key](const FRecentProjectInfo& Entry) { return MakeProjectKey(Entry.ProjectFile) == Key; });
		Entries.insert(Entries.begin(), {std::string(ProjectName), Normalized, ERecentProjectStatus::Available, {}});
		if (Entries.size() > MaximumRecentProjects) Entries.resize(MaximumRecentProjects);
		return Save(OutError);
	}

	auto FProjectHistory::Remove(std::string_view ProjectFile, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		const std::string Key = MakeProjectKey(ProjectFile);
		std::erase_if(Entries, [&Key](const FRecentProjectInfo& Entry) { return MakeProjectKey(Entry.ProjectFile) == Key; });
		return Save(OutError);
	}

	auto FProjectHistory::Save(std::string* OutError) const -> bool
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
		if (Document.SaveToFile(HistoryFile)) return true;
		SetError(OutError, std::format("Could not save project history '{}'.", HistoryFile));
		return false;
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
