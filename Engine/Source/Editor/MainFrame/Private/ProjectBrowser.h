#pragma once

#include "Misc/ProjectHistory.h"

namespace Durin::Editor::MainFrame
{
	// Draws and owns state for the startup project-selection window.
	class FProjectBrowser
	{
	public:
		using FOpenProject = std::function<bool(std::string_view, std::string&)>;

		FProjectBrowser();

		auto SetOpenProject(FOpenProject InOpenProject) -> void { OpenProject = std::move(InOpenProject); }
		auto SetError(std::string InError) -> void { Error = std::move(InError); }
		auto RecordCurrentProject() -> void;
		auto Draw() -> void;

	private:
		auto DrawBrandPanel(bool bCompact) -> void;
		auto DrawProjectContent() -> void;
		auto DrawRecentProjects(float Height) -> void;
		auto DrawProjectRow(size_t Index, const FRecentProjectInfo& Project) -> bool;
		auto OpenProjectFile(std::string_view ProjectFile) -> void;
		auto BrowseForProject() -> void;

		FProjectHistory History;
		FOpenProject OpenProject;
		std::string Error;
		int32 SelectedProject = -1;
	};
}
