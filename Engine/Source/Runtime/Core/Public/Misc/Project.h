#pragma once

#include "CoreAPI.h"
#include "Misc/ProjectError.h"

namespace Durin
{
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
	};

	// Describes how startup selects the process's initial project.
	struct FProjectInitializationParams
	{
		bool bOpenProjectBrowser = false;

		// An empty path falls back to project history unless the browser is requested.
		std::string RequestedProjectFile;
	};

	CORE_API auto GetCurrentProject() -> const FProjectInfo*;
	CORE_API auto HasCurrentProject() -> bool;
	[[nodiscard]] CORE_API auto InitializeCurrentProject(const FProjectInitializationParams& Params) -> std::expected<void, FProjectError>;
	CORE_API auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string;
	[[nodiscard]] CORE_API auto RelaunchEditorForProject(std::string_view ProjectFile) -> std::expected<void, FProjectError>;
	[[nodiscard]] CORE_API auto LaunchPendingEditorRelaunch() -> std::expected<void, FProjectError>;
	// Exclusively owns one project's edit session for this process lifetime.
	[[nodiscard]] CORE_API auto AcquireProjectEditOwnership() -> std::expected<void, FProjectError>;
	CORE_API auto ReleaseProjectEditOwnership() -> void;
}
