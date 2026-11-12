#pragma once

#include "CoreMinimal.h"

#include "ProjectsAPI.h"
#include "Project/ProjectError.h"

namespace Durin
{
	class FJsonNodeView;
	// Absent and empty Plugins objects both mean no explicit overrides.
	PROJECTS_API auto ReadProjectPluginOverrides(FJsonNodeView Root)
		-> std::expected<std::map<std::string, bool>, std::string>;

	// Holds the normalized paths and mount root of the active project.
	struct FProjectInfo
	{
		std::string Name;
		std::string ProjectFile;
		std::string ProjectDir;
		std::string ContentDir;
		std::string MountRoot;
		// Project-owned module roots enabled for this executable's runtime variant.
		std::vector<std::string> EnabledRootModules;
		std::map<std::string, bool> PluginOverrides;
		bool bRecordInteractiveHistory = false;
	};

	// Describes how startup selects the process's initial project.
	struct FProjectInitializationParams
	{
		bool bOpenProjectBrowser = false;

		// Empty selects no project. Interactive restoration is selected by the host.
		std::string RequestedProjectFile;
		bool bRecordInteractiveHistory = false;
	};

	PROJECTS_API auto GetCurrentProject() -> const FProjectInfo*;
	PROJECTS_API auto HasCurrentProject() -> bool;
	[[nodiscard]] PROJECTS_API auto InitializeCurrentProject(const FProjectInitializationParams& Params) -> std::expected<void, FProjectError>;
	PROJECTS_API auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string;
	PROJECTS_API auto RelaunchEditorForProject(std::string_view ProjectFile) -> void;
	PROJECTS_API auto TakePendingEditorRelaunchArguments() -> std::optional<std::vector<std::string>>;
	// Exclusively owns one project's edit session for this process lifetime.
	[[nodiscard]] PROJECTS_API auto AcquireProjectEditOwnership() -> std::expected<void, FProjectError>;
	PROJECTS_API auto ReleaseProjectEditOwnership() -> void;
}
