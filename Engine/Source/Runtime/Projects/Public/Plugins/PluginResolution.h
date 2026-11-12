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
		// Browser admission validates ordinary ownership without activating project overrides.
		bool bProjectBrowser = false;
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
		// Ordinary workspace module names allowed by the process loading boundary.
		std::vector<std::string> OrdinaryModules;
		std::vector<FResolvedPluginModule> Modules;
		// Canonical UTF-8 JSON with sorted object keys; excludes absolute paths.
		std::string ManifestJson;
		// Logical descriptor identity -> canonical machine-local file path.
		std::map<std::string, std::filesystem::path> Locations;
	};

	// Supplied by the host from its own build, never from the candidate inventory.
	struct FPluginBuildIdentity
	{
		std::string EngineBuild, Platform, Architecture, Configuration, RuntimeVariant, ToolchainABI;
	};
	struct FAdmittedPluginBinary
	{
		FResolvedPluginModule Module;
		std::filesystem::path Path;
		std::vector<std::filesystem::path> RuntimeDependencies;
	};

	// Read-only admission; publishes dependency/phase ordered paths only on success.
	[[nodiscard]] PROJECTS_API auto AdmitPluginBuild(
		const FPluginResolution& Selection, const FPluginBuildIdentity& Identity,
		const std::filesystem::path& InventoryFile,
		std::vector<FAdmittedPluginBinary>& OutBinaries, std::string& OutReason) -> bool;

	// Failures name the descriptor and the traversed dependency chain where applicable.
	[[nodiscard]] PROJECTS_API auto ResolvePlugins(const FPluginResolveRequest& Request)
		-> std::expected<FPluginResolution, std::string>;
}
