#include "Plugins/PluginManagement.h"
#include "Plugins/PluginResources.h"
#include "ProjectDescriptor.h"
#include "Project/Project.h"
#include "Project/ProjectContextInternal.h"
#include "Misc/FileHelper.h"
#include "Misc/StringHelper.h"

namespace Durin
{
    namespace
    {
        auto SetOverride(std::map<std::string, bool>& Values, std::string_view Id, std::optional<bool> Value) -> void
        {
            for (auto It = Values.begin(); It != Values.end();)
                if (StringUtils::FoldAscii(It->first) == StringUtils::FoldAscii(std::string(Id))) It = Values.erase(It);
                else ++It;
            if (Value) Values[std::string(Id)] = *Value;
        }
        auto ReadDisk(const std::filesystem::path& File) -> std::optional<std::string>
        {
            const auto Bytes = FFileHelper::LoadFileToArray(File);
            if (!Bytes) return {};
            return std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
        }
        auto IdSet(const std::vector<std::string>& Ids) -> std::set<std::string>
        {
            std::set<std::string> Result;
            for (const auto& Id : Ids) Result.insert(StringUtils::FoldAscii(Id));
            return Result;
        }
    }
    auto FPluginEditSession::Open(FPluginManagementContext InContext, std::string& OutReason) -> bool
    {
        FProjectDescriptor Descriptor;
        std::vector<FPluginInfo> Discovered;
        if (!Descriptor.Load(InContext.Request.ProjectFile.generic_string(), OutReason)
            || !DiscoverPlugins(InContext.Request, Discovered, OutReason)) return false;
        const auto Disk = ReadDisk(InContext.Request.ProjectFile);
        if (!Disk) { OutReason = "Cannot read project descriptor"; return false; }
        Context = std::move(InContext); Catalog = std::move(Discovered);
        Draft = Descriptor.PluginOverrides; DiskSnapshot = *Disk;
        // Missing references remain editable so a running editor can repair disk drift.
        for (const auto& [Id, Enabled] : Draft)
            if (std::ranges::none_of(Catalog, [&](const auto& Info) { return StringUtils::FoldAscii(Info.Id) == StringUtils::FoldAscii(Id); }))
                Catalog.push_back({Id, Id + " (missing descriptor)", "", {}, {}, {}});
        // Materialize the initial effective defaults as independent checkbox values.
        // Merely opening the window must not dirty or rewrite the descriptor.
        auto Request = Context.Request; Request.PluginOverrides = Draft;
        const auto Initial = ResolvePlugins(Request);
        const auto Defaults = Initial ? IdSet(Initial->Plugins) : std::set<std::string>{};
        Rows.clear();
        for (const auto& Info : Catalog)
        {
            const auto Existing = std::ranges::find_if(Draft, [&](const auto& Entry) { return StringUtils::FoldAscii(Entry.first) == StringUtils::FoldAscii(Info.Id); });
            const bool Requested = Existing == Draft.end() ? Defaults.contains(StringUtils::FoldAscii(Info.Id)) : Existing->second;
            SetOverride(Draft, Info.Id, Requested);
            FPluginManagementRow Row; Row.Info = Info;
            Row.bCompatible = !Info.DescriptorFile.empty() && (Info.Platforms.empty()
                || std::ranges::find(Info.Platforms, Context.Request.Platform) != Info.Platforms.end());
            Rows.push_back(std::move(Row));
        }
        Saved = Draft;
        RefreshBuildAvailability(); Rebuild(); OutReason.clear(); return true;
    }
    auto FPluginEditSession::Resolve(const std::map<std::string, bool>& Overrides, std::string& Reason) const -> std::optional<FPluginResolution>
    {
        auto Request = Context.Request; Request.PluginOverrides = Overrides;
        auto Result = ResolvePlugins(Request);
        if (!Result) { Reason = Result.error(); return {}; }
        FPluginResources Resources;
        if (!ResolvePluginResources(*Result, Resources, Reason)) return {};
        FProjectDescriptor Descriptor;
        if (!Descriptor.Load(Request.ProjectFile.generic_string(), Reason)) return {};
        auto Mounts = Descriptor.Mounts;
        Mounts.insert(Mounts.end(), Resources.ContentMounts.begin(), Resources.ContentMounts.end());
        if (!FMountPaths::ValidateMountDefinitions(BuildProjectMountDefinitions(Request.ProjectFile.generic_string(), Mounts), &Reason)) return {};
        Reason.clear(); return std::move(*Result);
    }
    auto FPluginEditSession::Available(const FPluginResolution& Selection, std::string& Reason) const -> bool
    {
        if (Selection.Plugins.empty()) { Reason.clear(); return true; }
        std::vector<FAdmittedPluginBinary> Binaries;
        return AdmitPluginBuild(Selection, Context.Identity, Context.InventoryFile, Binaries, Reason);
    }
    auto FPluginEditSession::RefreshBuildAvailability() -> void
    {
        // This is deliberately outside the checkbox path: resolution/admission reads disk.
        std::string Reason;
        const auto Selection = Resolve(Draft, Reason);
        for (auto& Row : Rows)
        {
            Row.Availability = Reason; Row.bBuildAvailable = false;
            std::set<std::string> Closure;
            const auto Include = [&](auto&& Self, std::string_view Id) -> void {
                const auto Key = StringUtils::FoldAscii(std::string(Id));
                if (!Closure.insert(Key).second) return;
                const auto Found = std::ranges::find_if(Catalog, [&](const FPluginInfo& Info) {
                    return StringUtils::FoldAscii(Info.Id) == Key;
                });
                if (Found != Catalog.end())
                    for (const auto& Dependency : Found->Dependencies) Self(Self, Dependency);
            };
            Include(Include, Row.Info.Id);
            std::optional<FPluginResolution> Preview;
            if (Selection && std::ranges::find_if(Selection->Plugins, [&](const std::string& Id) {
                return StringUtils::FoldAscii(Id) == StringUtils::FoldAscii(Row.Info.Id);
            }) != Selection->Plugins.end()) Preview = *Selection;
            else
            {
                std::map<std::string, bool> Overrides;
                for (const auto& Info : Catalog) Overrides[Info.Id] = Closure.contains(StringUtils::FoldAscii(Info.Id));
                Preview = Resolve(Overrides, Row.Availability);
            }
            if (!Preview) continue;
            FPluginResolution Subset = std::move(*Preview);
            std::erase_if(Subset.Plugins, [&](const std::string& Id) {
                return !Closure.contains(StringUtils::FoldAscii(Id));
            });
            std::erase_if(Subset.Modules, [&](const FResolvedPluginModule& Module) {
                return !Closure.contains(StringUtils::FoldAscii(Module.Owner));
            });
            Row.bBuildAvailable = Available(Subset, Row.Availability);
        }
    }
    auto FPluginEditSession::Rebuild() -> void
    {
        ValidationError.clear(); BuildError.clear();
        const auto Active = IdSet(Context.ActivePlugins);
        std::map<std::string, size_t> Index;
        for (size_t I = 0; I < Rows.size(); ++I)
        {
            auto& Row = Rows[I]; const auto Key = StringUtils::FoldAscii(Row.Info.Id);
            Index[Key] = I; Row.bActive = Active.contains(Key); Row.bNextStart = false;
            Row.Reason.clear(); Row.Override.reset();
            for (const auto& [Id, Enabled] : Draft) if (StringUtils::FoldAscii(Id) == Key) Row.Override = Enabled;
            Row.bRequested = Row.Override.value_or(false);
        }
        std::vector<int> State(Rows.size());
        const auto Visit = [&](auto&& Self, size_t I) -> bool {
            auto& Row = Rows[I];
            if (State[I] == 2) return Row.bNextStart;
            if (State[I] == 1) { ValidationError = "Plugin dependency cycle: " + Row.Info.Id; return false; }
            State[I] = 1;
            bool Effective = Row.bRequested && Row.bCompatible;
            if (Row.bRequested && !Row.bCompatible) Row.Reason = "Inactive: incompatible or missing descriptor";
            if (Effective)
                for (const auto& Dependency : Row.Info.Dependencies)
                {
                    const auto Found = Index.find(StringUtils::FoldAscii(Dependency));
                    if (Found == Index.end() || !Self(Self, Found->second))
                    { Effective = false; Row.Reason = "Inactive: requires " + Dependency; }
                }
            State[I] = 2; Row.bNextStart = Effective; return Effective;
        };
        std::set<std::string> Next;
        for (size_t I = 0; I < Rows.size(); ++I)
            if (Visit(Visit, I))
            {
                Next.insert(StringUtils::FoldAscii(Rows[I].Info.Id));
                if (!Rows[I].bBuildAvailable && BuildError.empty()) BuildError = Rows[I].Availability;
            }
        bRestartRequired = Active != Next;
    }
    auto FPluginEditSession::RequestOverride(std::string_view Id, std::optional<bool> Enabled,
        std::vector<std::string>& OutEffects, std::string& OutReason) -> bool
    {
        const auto Info = std::ranges::find_if(Catalog, [&](const auto& Item) { return StringUtils::FoldAscii(Item.Id) == StringUtils::FoldAscii(std::string(Id)); });
        if (Info == Catalog.end()) { OutReason = "Unknown plugin"; return false; }
        std::set<std::string> Before;
        for (const auto& Row : Rows) if (Row.bNextStart) Before.insert(Row.Info.Id);
        SetOverride(Draft, Info->Id, Enabled); Rebuild();
        OutEffects.clear();
        for (const auto& Row : Rows)
            if (Before.contains(Row.Info.Id) != Row.bNextStart)
                OutEffects.push_back(Row.Info.Id + (Row.bNextStart ? " will be enabled" : " will be inactive"));
        OutReason.clear(); return true;
    }
    auto FPluginEditSession::SetEnabled(std::string_view Id, bool Enabled, std::string& OutReason) -> bool
    {
        std::vector<std::string> Effects;
        return RequestOverride(Id, Enabled, Effects, OutReason);
    }
    auto FPluginEditSession::Save(std::string& OutReason) -> bool
    {
        const auto* Project = GetCurrentProject();
        if (!Project || NormalizeProjectFile(Context.Request.ProjectFile.generic_string()) != Project->ProjectFile)
        { OutReason = "The plugin edit session does not belong to the active project"; return false; }
        if (!SaveProjectPluginOverrides(Draft, DiskSnapshot, OutReason)) return false;
        const auto Disk = ReadDisk(Context.Request.ProjectFile);
        if (!Disk) { OutReason = "Saved descriptor cannot be reread; refresh plugin management"; return false; }
        DiskSnapshot = *Disk; Saved = Draft; RefreshBuildAvailability(); Rebuild(); OutReason.clear(); return true;
    }
    auto FPluginEditSession::Revert(std::string& OutReason) -> bool
    {
        Draft = Saved; Rebuild(); OutReason.clear(); return true;
    }
    auto FPluginEditSession::CheckRestartReady(std::string& OutReason) -> bool
    {
        if (IsDirty()) { OutReason = "Save plugin changes before restarting"; return false; }
        const auto Disk = ReadDisk(Context.Request.ProjectFile);
        if (!Disk || *Disk != DiskSnapshot) { OutReason = "Project descriptor changed; refresh before restarting"; return false; }
        const auto Selection = Resolve(Saved, OutReason);
        if (!Selection) return false;
        const bool Ready = Available(*Selection, OutReason);
        RefreshBuildAvailability(); Rebuild();
        return Ready;
    }
}
