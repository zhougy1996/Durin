#include "Plugins/PluginManagement.h"
#include "Plugins/PluginResources.h"
#include "Project/Project.h"
#include "ProjectDescriptor.h"
#include "Project/ProjectContextInternal.h"
#include "Misc/StringHelper.h"

namespace Durin
{
    auto ResolveAvailablePlugins(const FPluginResolveRequest& Request,
        const FPluginBuildIdentity& Identity, const std::filesystem::path& Inventory,
        FPluginResolution& OutSelection, std::vector<FAdmittedPluginBinary>& OutBinaries,
        FPluginResources& OutResources, std::vector<std::string>& OutDiagnostics, std::string& OutReason) -> bool
    {
        std::vector<FPluginInfo> Catalog;
        if (!DiscoverPlugins(Request, Catalog, OutReason)) return false;
        FProjectDescriptor Project;
        if (!Project.Load(Request.ProjectFile.generic_string(), OutReason)) return false;
        auto Loaded = LoadPluginStartupOverrides(Request.ProjectFile);
        std::map<std::string, bool> Requests;
        if (Loaded) Requests = std::move(*Loaded);
        else OutDiagnostics.push_back(Loaded.error());
        for (const auto& Id : Project.PluginDependencies)
        {
            std::erase_if(Requests, [&](const auto& Item) { return StringUtils::FoldAscii(Item.first) == StringUtils::FoldAscii(Id); });
            Requests[Id] = true;
        }
        auto Preview = Request;
        Preview.bRecoverySelection = true;
        std::map<std::string, bool> Accepted;
        for (const auto& [Id, Enabled] : Requests) if (!Enabled) Accepted[Id] = false;
        // Unknown disabled choices cannot make otherwise usable plugins fail.
        std::erase_if(Accepted, [&](const auto& Item) {
            return std::ranges::none_of(Catalog, [&](const auto& Info) { return StringUtils::FoldAscii(Info.Id) == StringUtils::FoldAscii(Item.first); });
        });
        for (const auto& [Id, Enabled] : Requests)
        {
            if (!Enabled) continue;
            auto Candidate = Accepted;
            Candidate[Id] = true;
            Preview.PluginOverrides = Candidate;
            const auto Selection = ResolvePlugins(Preview);
            std::vector<FAdmittedPluginBinary> Binaries;
            FPluginResources Resources;
            std::string Error;
            const auto IsUsable = [&] {
                if (!Selection) { Error = Selection.error(); return false; }
                if (std::ranges::none_of(Selection->Plugins, [&](const auto& Name) { return StringUtils::FoldAscii(Name) == StringUtils::FoldAscii(Id); }))
                { Error = "a required plugin is disabled"; return false; }
                if (!AdmitPluginBuild(*Selection, Identity, Inventory, Binaries, Error)
                    || !ResolvePluginResources(*Selection, Resources, Error)) return false;
                auto Mounts = Project.Mounts;
                Mounts.insert(Mounts.end(), Resources.ContentMounts.begin(), Resources.ContentMounts.end());
                return FMountPaths::ValidateMountDefinitions(BuildProjectMountDefinitions(Request.ProjectFile.generic_string(), Mounts), &Error);
            };
            if (IsUsable()) { Accepted = std::move(Candidate); continue; }
            OutDiagnostics.push_back("Plugin " + Id + " could not be enabled: " + Error);
        }
        Preview.PluginOverrides = Accepted;
        auto Selection = ResolvePlugins(Preview);
        if (!Selection) { OutReason = Selection.error(); return false; }
        std::vector<FAdmittedPluginBinary> Binaries;
        FPluginResources Resources;
        if (!Selection->Plugins.empty() && !AdmitPluginBuild(*Selection, Identity, Inventory, Binaries, OutReason)) return false;
        if (!ResolvePluginResources(*Selection, Resources, OutReason)) return false;
        OutSelection = std::move(*Selection);
        OutBinaries = std::move(Binaries);
        OutResources = std::move(Resources);
        OutReason.clear();
        return true;
    }
}
