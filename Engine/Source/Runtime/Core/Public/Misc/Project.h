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

	CORE_API auto GetCurrentProject() -> const FProjectInfo*;
	CORE_API auto HasCurrentProject() -> bool;
	CORE_API auto InitializeCurrentProject(std::span<const std::string_view> Arguments, std::string* OutError = nullptr) -> bool;
	CORE_API auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string;
	CORE_API auto RelaunchEditorForProject(std::string_view ProjectFile, std::string* OutError = nullptr) -> bool;
	CORE_API auto LaunchPendingEditorRelaunch(std::string* OutError = nullptr) -> bool;
}
