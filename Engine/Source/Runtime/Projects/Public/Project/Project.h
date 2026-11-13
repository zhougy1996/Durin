#pragma once

#include "CoreMinimal.h"

#include "ProjectsAPI.h"
#include "Project/ProjectError.h"

namespace Durin
{
	class FJsonNodeView;
	// Startup choices live in Configs/Plugins.json, independently of the build graph.
	PROJECTS_API auto ReadProjectPluginOverrides(FJsonNodeView Root)
		-> std::expected<std::map<std::string, bool>, std::string>;

	PROJECTS_API auto PluginStartupConfigFile(const std::filesystem::path& ProjectFile) -> std::filesystem::path;
	// An absent file is represented by an empty snapshot; existing unreadable files fail.
	PROJECTS_API auto ReadPluginStartupSnapshot(const std::filesystem::path& ProjectFile)
		-> std::expected<std::string, std::string>;
	PROJECTS_API auto LoadPluginStartupOverrides(const std::filesystem::path& ProjectFile)
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
		std::vector<std::string> PluginDependencies;
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
