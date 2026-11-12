#pragma once

#include "CoreMinimal.h"
#include "ProjectsAPI.h"

namespace Durin
{
	// Value-only query: no global registration, native loading, or mount publication.
	struct FPluginResolveRequest
	{
		std::filesystem::path EngineRoot;
		std::filesystem::path ProjectFile;
		// Include every workspace project to validate module ownership and graph edges.
		std::vector<std::filesystem::path> ProjectFiles;
		std::string Platform = "Win64";
		std::string RuntimeVariant = "DurinEditor";
	};

	struct FResolvedPluginModule
	{
		std::string Name;
		std::string Owner;
		std::string Host;
		std::string Phase;
	};
	struct FPluginResolution
	{
		std::vector<std::string> Plugins;
		std::vector<FResolvedPluginModule> Modules;
		// Canonical UTF-8 JSON with sorted object keys; excludes absolute paths.
		std::string ManifestJson;
		// Logical descriptor identity -> canonical machine-local file path.
		std::map<std::string, std::filesystem::path> Locations;
	};

	// Failures name the descriptor and the traversed dependency chain where applicable.
	[[nodiscard]] PROJECTS_API auto ResolvePlugins(const FPluginResolveRequest& Request)
		-> std::expected<FPluginResolution, std::string>;
}
