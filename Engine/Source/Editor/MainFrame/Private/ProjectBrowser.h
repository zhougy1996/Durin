#pragma once

#include "Misc/ProjectHistory.h"

namespace Durin
{
	class FRHITexture;
}

namespace Durin::Editor::MainFrame
{
	// Draws and owns state for the startup project-selection window.
	class FProjectBrowser
	{
	public:
		using FOpenProject = std::function<bool(std::string_view, std::string&)>;
		using FClose = std::function<void()>;

		FProjectBrowser();

		auto SetOpenProject(FOpenProject InOpenProject) -> void { OpenProject = std::move(InOpenProject); }
		auto SetClose(FClose InClose) -> void { Close = std::move(InClose); }
		auto SetError(std::string InError) -> void { Error = std::move(InError); }
		auto RecordCurrentProject() -> void;
		auto Draw(const FRHITexture* BrandTexture, bool bCanClose = false) -> void;

	private:
		auto DrawBrandPanel(bool bCompact, const FRHITexture* BrandTexture) -> void;
		auto DrawProjectContent(bool bCanClose) -> void;
		auto DrawRecentProjects(float Height) -> void;
		auto DrawProjectRow(size_t Index, const FRecentProjectInfo& Project) -> bool;
		auto OpenProjectFile(std::string_view ProjectFile) -> void;
		auto BrowseForProject() -> void;

		FProjectHistory History;
		FOpenProject OpenProject;
		FClose Close;
		std::string Error;
		int32 SelectedProject = -1;
	};
}
