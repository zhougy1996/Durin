#pragma once
#include "CoreMinimal.h"
#include "ProjectsAPI.h"
#include "Plugins/PluginResources.h"

namespace Durin
{
    // Detached source metadata/configuration captured before offline Cook discovery.
    PROJECTS_API auto CapturePluginCookSnapshot(std::string_view Platform, std::string& OutJson, std::string& OutReason) -> bool;
    PROJECTS_API auto PluginFileSha256(const std::filesystem::path& File) -> std::string;
    // No source descriptors or workspace access; publishes outputs only after full admission.
    PROJECTS_API auto ReadPluginRuntimeInventory(const std::filesystem::path& File, std::string_view ProjectFile,
        const FPluginBuildIdentity& Host, FPluginResolution& OutSelection,
        std::vector<FAdmittedPluginBinary>& OutBinaries, FPluginResources& OutResources, std::string& OutReason) -> bool;
}
