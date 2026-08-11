#pragma once

#include "CoreAPI.h"

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
	CORE_API auto InitializeCurrentProject(const FProjectInitializationParams& Params, std::string* OutError = nullptr) -> bool;
	CORE_API auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string;
	CORE_API auto RelaunchEditorForProject(std::string_view ProjectFile, std::string* OutError = nullptr) -> bool;
	CORE_API auto LaunchPendingEditorRelaunch(std::string* OutError = nullptr) -> bool;
	// Exclusively owns one project's authoring session for this process lifetime.
	CORE_API auto AcquireProjectAuthoringOwnership(std::string* OutError = nullptr) -> bool;
	CORE_API auto ReleaseProjectAuthoringOwnership() -> void;
}
