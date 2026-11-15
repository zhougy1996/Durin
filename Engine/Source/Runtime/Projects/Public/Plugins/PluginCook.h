#pragma once
#include "CoreMinimal.h"
#include "ProjectsAPI.h"
#include "Plugins/PluginResources.h"

namespace Durin
{
    struct FGameCookTarget
    {
        FPluginBuildIdentity Identity;
        std::string Project, Compatibility, ReceiptSha256;
    };
    PROJECTS_API auto ReadGameCookTarget(const std::filesystem::path& Receipt,std::string_view Platform,
        FGameCookTarget& OutTarget,std::string& OutReason) -> bool;
    // Detached source metadata/configuration captured before offline Cook discovery.
    PROJECTS_API auto CapturePluginCookSnapshot(std::string_view Platform, const FGameCookTarget& Target, std::string& OutJson, std::string& OutReason) -> bool;
    // Projects the captured source evidence to the minimal, independently versioned runtime bindings.
    PROJECTS_API auto MakeRuntimeCookSnapshot(std::string_view EvidenceJson, std::string& OutJson, std::string& OutReason) -> bool;
    PROJECTS_API auto PluginFileSha256(const std::filesystem::path& File) -> std::string;
    // No source descriptors or workspace access; publishes outputs only after full admission.
    PROJECTS_API auto ReadPluginRuntimeInventory(const std::filesystem::path& File, std::string_view ProjectFile,
        const FPluginBuildIdentity& Host, FPluginResolution& OutSelection,
        std::vector<FAdmittedPluginBinary>& OutBinaries, FPluginResources& OutResources, std::string& OutReason) -> bool;
}
