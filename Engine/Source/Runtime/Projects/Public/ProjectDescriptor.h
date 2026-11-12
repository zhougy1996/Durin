#pragma once

#include "CoreMinimal.h"
#include "ProjectsAPI.h"
#include "Json/Json.h"
#include "Misc/MountPaths.h"

namespace Durin
{
	// Persistent project metadata; inspection does not alter the active process.
	class FProjectDescriptor
	{
	public:
		std::string Name;
		std::map<std::string, std::string> ModuleDirs;
		std::map<std::string, bool> PluginOverrides;
		std::vector<FMountPoint> Mounts;
		std::optional<std::vector<std::string>> BaseModules;
		std::map<std::string,std::vector<std::string>> ExtraModules;
		std::optional<std::string> NativeTestRoot;
		std::string Schema;
		PROJECTS_API auto Load(std::string_view File, std::string& OutFailReason) -> bool;
		PROJECTS_API auto Read(const FJsonDocument& Source, std::string_view File, std::string& OutFailReason) -> bool;
		PROJECTS_API auto GetEnabledRootModules(std::string_view Variant) const
			-> std::expected<std::vector<std::string>, std::string>;
		PROJECTS_API auto Save(std::string_view File, std::string& OutFailReason) const -> bool;
	private:
		std::vector<std::string> ModuleOrder;

	};
	struct FProjectStatus
	{
		std::string Name;
		std::string ProjectFile;
	};
}
