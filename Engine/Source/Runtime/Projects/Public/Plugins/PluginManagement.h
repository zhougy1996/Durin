#pragma once

#include "CoreMinimal.h"
#include "ProjectsAPI.h"
#include "Plugins/PluginResolution.h"

namespace Durin
{
    struct FPluginInfo
    {
        std::string Id, DisplayName, Version;
        std::vector<std::string> Dependencies, Platforms;
        std::filesystem::path DescriptorFile;
    };
    PROJECTS_API auto DiscoverPlugins(const FPluginResolveRequest& Request,
        std::vector<FPluginInfo>& OutPlugins, std::string& OutReason) -> bool;
    struct FPluginManagementContext
    {
        FPluginResolveRequest Request;
        FPluginBuildIdentity Identity;
        std::filesystem::path InventoryFile;
        std::vector<std::string> ActivePlugins;
    };
    PROJECTS_API auto MakePluginManagementContext(FPluginManagementContext& OutContext, std::string& OutReason) -> bool;
    PROJECTS_API auto GetActivePluginIds() -> std::span<const std::string>;
    // Atomic manager-owned edit/save. A stale UI session cannot overwrite later descriptor edits.
    PROJECTS_API auto SaveProjectPluginOverrides(const std::map<std::string, bool>& Overrides,
        std::string_view ExpectedDisk, std::string& OutReason) -> bool;

    struct FPluginManagementRow
    {
        FPluginInfo Info;
        std::optional<bool> Override;
        bool bRequested = false, bActive = false, bNextStart = false, bCompatible = true, bBuildAvailable = false;
        std::string Reason, Availability;
    };
    class FPluginEditSession
    {
    public:
        PROJECTS_API auto Open(FPluginManagementContext Context, std::string& OutReason) -> bool;
        PROJECTS_API auto RequestOverride(std::string_view Id, std::optional<bool> Enabled,
            std::vector<std::string>& OutEffects, std::string& OutReason) -> bool;
        PROJECTS_API auto SetEnabled(std::string_view Id, bool Enabled, std::string& OutReason) -> bool;
        PROJECTS_API auto Save(std::string& OutReason) -> bool;
        PROJECTS_API auto Revert(std::string& OutReason) -> bool;
        PROJECTS_API auto CheckRestartReady(std::string& OutReason) -> bool;
        auto GetRows() const -> const std::vector<FPluginManagementRow>& { return Rows; }
        auto GetOverrides() const -> const std::map<std::string, bool>& { return Draft; }
        auto IsDirty() const -> bool { return Draft != Saved; }
        auto RequiresRestart() const -> bool { return bRestartRequired; }
        auto IsSelectionValid() const -> bool { return ValidationError.empty(); }
        auto GetValidationError() const -> const std::string& { return ValidationError; }
        auto GetBuildError() const -> const std::string& { return BuildError; }
        auto GetProjectFile() const -> const std::filesystem::path& { return Context.Request.ProjectFile; }
    private:
        FPluginManagementContext Context;
        std::vector<FPluginInfo> Catalog;
        std::vector<FPluginManagementRow> Rows;
        std::map<std::string, bool> Saved, Draft;
        std::string DiskSnapshot, ValidationError, BuildError;
        bool bRestartRequired = false;
        auto Rebuild() -> void;
        auto RefreshBuildAvailability() -> void;
        auto Resolve(const std::map<std::string, bool>& Overrides, std::string& Reason) const -> std::optional<FPluginResolution>;
        auto Available(const FPluginResolution& Selection, std::string& Reason) const -> bool;
    };
}
