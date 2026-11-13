#pragma once

#include "CoreMinimal.h"
#include "ProjectsAPI.h"
#include "Plugins/PluginResolution.h"
#include "Misc/MountPaths.h"
#include "Yaml/Yaml.h"

namespace Durin
{
    struct FPluginShaderRoot
    {
        std::string VirtualRoot;
        std::filesystem::path SourceDirectory;
    };
    struct FPluginResources
    {
        std::vector<FMountPoint> ContentMounts;
        std::vector<FPluginShaderRoot> ShaderRoots;
        std::vector<std::filesystem::path> ConfigDirectories;
    };
    // Read-only preparation; output is published only after all roots validate.
    PROJECTS_API auto ResolvePluginResources(const FPluginResolution& Selection,
        FPluginResources& OutResources, std::string& OutReason) -> bool;
    // Admit independent usable closures; failed plugins remain requested on disk.
    PROJECTS_API auto ResolveAvailablePlugins(const FPluginResolveRequest& Request,
        const FPluginBuildIdentity& Identity, const std::filesystem::path& Inventory,
        FPluginResolution& OutSelection, std::vector<FAdmittedPluginBinary>& OutBinaries,
        FPluginResources& OutResources, std::vector<std::string>& OutDiagnostics, std::string& OutReason) -> bool;
    // Frozen at process admission, independent of pending descriptor edits.
    PROJECTS_API auto GetActivePluginResources() -> const FPluginResources&;
    // Reads defaults in dependency order, then the authored project file. Never saves defaults.
    PROJECTS_API auto LoadProjectConfig(const std::filesystem::path& File,
        FYamlDocument& OutDocument, std::string& OutReason) -> bool;
}
