#include "PluginStartup.h"
#include "Plugins/PluginResources.h"
#include "Plugins/PluginCook.h"
#include "Plugins/PluginManagement.h"
#include "Misc/AppConfig.h"
#include "Project/Project.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"
#include "Misc/Build.h"
#include "Misc/Version.h"
#include "Misc/FileHelper.h"
#include "ProjectDescriptor.h"
#include "OrdinaryModules.gen.h"
#if defined(_WIN32)
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#endif

namespace Durin
{
	namespace
	{
		auto HostCoreHash() -> std::string
		{
			std::filesystem::path CorePath;
#if defined(_WIN32)
			HMODULE Handle = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetEngineVersionString), &Handle)) throw std::runtime_error("Cannot locate loaded Core");
			std::vector<wchar_t> Buffer(32768);
			const auto Length = GetModuleFileNameW(Handle, Buffer.data(), static_cast<DWORD>(Buffer.size()));
			if (!Length || Length >= Buffer.size()) throw std::runtime_error("Cannot locate loaded Core path");
			CorePath = std::wstring(Buffer.data(), Length);
#elif defined(__APPLE__)
			Dl_info Info{};
			if (!dladdr(reinterpret_cast<void*>(&GetEngineVersionString), &Info)) throw std::runtime_error("Cannot locate loaded Core");
			CorePath = Info.dli_fname;
#endif
            return PluginFileSha256(CorePath);
		}
	}
    auto ResolvePluginResources(const FPluginResolution& Selection,
        FPluginResources& OutResources, std::string& OutReason) -> bool
    {
        try
        {
            FPluginResources Result;
            std::map<std::string, std::vector<std::string>> Dependencies;
            std::map<std::string, std::string> ContentRoots;
            for (const auto& Id : Selection.Plugins)
            {
                const auto Descriptor = Selection.Locations.at("Plugin/" + Id);
                const auto Owner = std::filesystem::canonical(Descriptor).parent_path();
                FJsonDocument Document;
                if (!Document.LoadFromFile(Descriptor)) throw std::runtime_error("Cannot read plugin resources: " + Descriptor.generic_string());
                const auto Root = Document.GetRootView();
                const auto Deps = Root.GetView("Dependencies");
                for (size_t I = 0; I < Deps.Num(); ++I)
                {
                    const auto Name = Deps.GetView(I).GetString();
                    const auto Found = std::ranges::find_if(Selection.Plugins, [&](const auto& Candidate) {
                        return StringUtils::FoldAscii(Candidate) == StringUtils::FoldAscii(Name); });
                    if (Found == Selection.Plugins.end()) throw std::runtime_error("Unselected plugin dependency: " + Name);
                    Dependencies[Id].push_back(*Found);
                }
                const auto Resource = [&](const char* Field) -> std::filesystem::path {
                    const auto Node = Root.GetView(Field);
                    if (!Node.IsValid()) return {};
                    const auto Relative = std::filesystem::path(Node.GetString());
                    if (Relative.empty() || Relative.is_absolute() || Relative.has_root_name()
                        || std::ranges::any_of(Relative, [](const auto& Part) { return Part == ".."; }))
                        throw std::runtime_error("Invalid plugin resource path: " + Id + "/" + Field);
                    const auto Path = std::filesystem::canonical(Owner / Relative);
                    const auto Relation = Path.lexically_relative(Owner);
                    if (Relation.empty() || *Relation.begin() == ".." || !std::filesystem::is_directory(Path))
                        throw std::runtime_error("Plugin resource directory escapes its owner or is unavailable: " + Id + "/" + Field);
                    return Path;
                };
                const auto Virtual = "/Plugins/" + Id + "/";
                if (const auto Content = Resource("Content"); !Content.empty())
                {
                    ContentRoots[Id] = Virtual;
                    Result.ContentMounts.push_back({.VirtualRoot=Virtual, .Owner=EMountOwner::Extension,
                        .Root=Owner, .ContentPath=Content.lexically_relative(Owner), .bAutoScan=true,
                        .bContentWritable=false, .Dependencies={"/Engine/"}});
                }
                if (const auto Shaders = Resource("Shaders"); !Shaders.empty()) Result.ShaderRoots.push_back({Virtual, Shaders});
                if (const auto Configs = Resource("Configs"); !Configs.empty())
                {
                    for (const auto* Name : {"Project.yaml", "DurinEditor.yaml", "DurinGame.yaml"})
                    {
                        const auto File = Configs / Name;
                        if (!std::filesystem::exists(File)) continue;
                        const auto Relation = std::filesystem::canonical(File).lexically_relative(Configs);
                        if (Relation.empty() || *Relation.begin() == "..") throw std::runtime_error("Plugin config escapes its root: " + File.generic_string());
                        FYamlDocument Parsed; std::string Error;
                        const std::array Files{File};
                        if (!LoadConfigLayers(Files, Parsed, Error)) throw std::runtime_error(Error);
                    }
                    Result.ConfigDirectories.push_back(Configs);
                }
            }
            for (auto& Mount : Result.ContentMounts)
            {
                const auto Id = Mount.VirtualRoot.substr(9, Mount.VirtualRoot.size() - 10);
                std::set<std::string> Seen;
                const auto Visit = [&](auto&& Self, const std::string& Owner) -> void {
                    for (const auto& Dependency : Dependencies[Owner])
                    {
                        if (!Seen.insert(Dependency).second) continue;
                        if (const auto It = ContentRoots.find(Dependency); It != ContentRoots.end()) Mount.Dependencies.push_back(It->second);
                        Self(Self, Dependency);
                    }
                };
                Visit(Visit, Id);
            }
            OutResources = std::move(Result);
            OutReason.clear();
            return true;
        }
        catch (const std::exception& Error) { OutReason = std::string("Plugin resource admission failed: ") + Error.what(); return false; }
    }

    auto MakePluginManagementContext(FPluginManagementContext& OutContext, std::string& OutReason) -> bool
    {
        try
        {
            const auto* Project = GetCurrentProject();
            if (!Project) { OutReason = "Open a project to manage plugins"; return false; }
            FPluginManagementContext Result;
            Result.Request.EngineRoot = FPaths::EngineDir(); Result.Request.ProjectFile = Project->ProjectFile;
            Result.Request.Platform = DURIN_BUILD_PLATFORM_STRING; Result.Request.RuntimeVariant = DURIN_RUNTIME_VARIANT;
            FJsonDocument Workspace;
            if (!Workspace.LoadFromFile(std::filesystem::path(FPaths::RootDir()) / "Durin.dworkspace"))
            { OutReason = "Source plugin management requires a readable workspace"; return false; }
            const auto Files = Workspace.GetRootView().GetView("Projects");
            if (!Files.IsArray()) { OutReason = "Invalid workspace project list"; return false; }
            for (size_t I = 0; I < Files.Num(); ++I) Result.Request.ProjectFiles.push_back(std::filesystem::path(FPaths::RootDir()) / Files.GetView(I).GetString());
            static const std::string CoreHash = HostCoreHash();
            Result.Identity = {std::string(GetEngineVersionString()) + ":" + CoreHash, Result.Request.Platform,
                DURIN_PLUGIN_ARCHITECTURE, DURIN_BUILD_TYPE_STRING, Result.Request.RuntimeVariant, DURIN_PLUGIN_TOOLCHAIN};
            Result.InventoryFile = std::filesystem::path(DURIN_PLUGIN_BUILD_ROOT) / Project->Name / DURIN_BUILD_TYPE_STRING / "PluginBuild.json";
            const auto Active = GetActivePluginIds(); Result.ActivePlugins.assign(Active.begin(), Active.end());
            OutContext = std::move(Result); OutReason.clear(); return true;
        }
        catch (const std::exception& Error) { OutReason = Error.what(); return false; }
    }

	auto ResolveProcessPlugins(std::string_view ProjectFile, FPluginResolution& OutSelection,
		std::vector<FAdmittedPluginBinary>& OutBinaries, FPluginResources& OutResources, std::string& OutReason,
        bool bAllowUnavailablePlugins, std::vector<std::string>& OutDiagnostics) -> bool
	{
		try
		{
#if !DURIN_WITH_EDITOR
            const auto RuntimeInventory=std::filesystem::path(FPaths::LaunchDir())/"PluginRuntime.json";
            if (!std::filesystem::exists(RuntimeInventory) && std::filesystem::exists(std::filesystem::path(FPaths::LaunchDir())/"PluginCook.json"))
                throw std::runtime_error("Cooked Game plugin runtime inventory is missing; restage the Game");
            if (std::filesystem::exists(RuntimeInventory))
            {
                FPluginBuildIdentity Host{std::string(GetEngineVersionString())+":"+HostCoreHash(),DURIN_BUILD_PLATFORM_STRING,
                    DURIN_PLUGIN_ARCHITECTURE,DURIN_BUILD_TYPE_STRING,DURIN_RUNTIME_VARIANT,DURIN_PLUGIN_TOOLCHAIN};
                if (!ReadPluginRuntimeInventory(RuntimeInventory,ProjectFile,Host,OutSelection,OutBinaries,OutResources,OutReason)) return false;
                for (const auto* Name:GOrdinaryModuleNames) OutSelection.OrdinaryModules.emplace_back(Name);
                return true;
            }
#endif
			FPluginResolveRequest Request;
			Request.EngineRoot = FPaths::EngineDir();
			Request.bProjectBrowser = ProjectFile.empty();
			Request.ProjectFile = ProjectFile.empty() ? Request.EngineRoot / "Engine.dproject" : std::filesystem::path(ProjectFile);
			Request.Platform = DURIN_BUILD_PLATFORM_STRING;
			Request.RuntimeVariant = DURIN_RUNTIME_VARIANT;
			const auto WorkspaceRoot = std::filesystem::path(FPaths::RootDir());
			// Browser is independent of plugin metadata. Existing no-plugin staged
			// Games also retain startup without source workspace descriptors.
			if (ProjectFile.empty() || !std::filesystem::is_regular_file(WorkspaceRoot / "Durin.dworkspace"))
			{
				if (!ProjectFile.empty())
				{
					FProjectDescriptor Descriptor;
					if (!Descriptor.Load(ProjectFile, OutReason)) return false;
					const auto Overrides = LoadPluginStartupOverrides(std::filesystem::path(ProjectFile));
                    if (!Overrides) throw std::runtime_error(Overrides.error());
                    if (!Descriptor.PluginDependencies.empty() || std::ranges::any_of(*Overrides, [](const auto& Entry) { return Entry.second; })) throw std::runtime_error("Source plugin admission requires workspace descriptors; staged plugin deployment is not available");
				}
				FPluginResolution Empty;
				for (const auto* Name : GOrdinaryModuleNames) Empty.OrdinaryModules.emplace_back(Name);
				OutSelection = std::move(Empty);
				OutBinaries.clear();
				OutReason.clear();
				return true;
			}
			FJsonDocument Workspace;
			const auto WorkspaceFile = WorkspaceRoot / "Durin.dworkspace";
			if (!Workspace.LoadFromFile(WorkspaceFile)) throw std::runtime_error("Cannot read workspace: " + WorkspaceFile.generic_string());
			const auto Projects = Workspace.GetRootView().GetView("Projects");
			if (!Projects.IsArray()) throw std::runtime_error("Invalid workspace Projects");
			for (size_t I = 0; I < Projects.Num(); ++I)
			{
				if (!Projects.GetView(I).IsString()) throw std::runtime_error("Invalid workspace project path");
				Request.ProjectFiles.push_back(WorkspaceRoot / Projects.GetView(I).GetString());
			}
            if (bAllowUnavailablePlugins && !ProjectFile.empty())
            {
                FProjectDescriptor Project;
                if (!Project.Load(ProjectFile, OutReason)) throw std::runtime_error(OutReason);
                FPluginBuildIdentity Identity{std::string(GetEngineVersionString()) + ":" + HostCoreHash(),
                    Request.Platform, DURIN_PLUGIN_ARCHITECTURE, DURIN_BUILD_TYPE_STRING, Request.RuntimeVariant, DURIN_PLUGIN_TOOLCHAIN};
                const auto Inventory = std::filesystem::path(DURIN_PLUGIN_BUILD_ROOT) / Project.Name / DURIN_BUILD_TYPE_STRING / "PluginBuild.json";
                if (!ResolveAvailablePlugins(Request, Identity, Inventory, OutSelection, OutBinaries, OutResources, OutDiagnostics, OutReason))
                    throw std::runtime_error(OutReason);
                return true;
            }
			auto Selected = ResolvePlugins(Request);
			if (!Selected) { OutReason = Selected.error(); return false; }
			std::vector<FAdmittedPluginBinary> Binaries;
			if (!ProjectFile.empty() && !Selected->Plugins.empty())
			{
				FJsonDocument Project;
				if (!Project.LoadFromFile(Request.ProjectFile)) throw std::runtime_error("Cannot read project");
				const auto Name = Project.GetRootView().GetView("ProjectName").GetString();
				FPluginBuildIdentity Identity{std::string(GetEngineVersionString()) + ":" + HostCoreHash(),
					Request.Platform, DURIN_PLUGIN_ARCHITECTURE, DURIN_BUILD_TYPE_STRING,
					Request.RuntimeVariant, DURIN_PLUGIN_TOOLCHAIN};
				const auto Inventory = std::filesystem::path(DURIN_PLUGIN_BUILD_ROOT) / Name / DURIN_BUILD_TYPE_STRING / "PluginBuild.json";
				if (!AdmitPluginBuild(*Selected, Identity, Inventory, Binaries, OutReason)) return false;
			}
			// Browser never starts project plugins, including engine-project overrides.
			if (ProjectFile.empty()) { Selected->Plugins.clear(); Selected->Modules.clear(); }
            if (!ResolvePluginResources(*Selected,OutResources,OutReason)) return false;
			OutSelection = std::move(*Selected);
			OutBinaries = std::move(Binaries);
			OutReason.clear();
			return true;
		}
		catch (const std::exception& Error)
		{
            if (bAllowUnavailablePlugins)
            {
                OutDiagnostics.push_back(std::string("Plugin startup failed; plugins were skipped: ") + Error.what());
                OutSelection = {};
                for (const auto* Name : GOrdinaryModuleNames) OutSelection.OrdinaryModules.emplace_back(Name);
                OutBinaries.clear(); OutResources = {}; OutReason.clear();
                return true;
            }
			OutReason = std::string(Error.what()) + "; configure/build required before restart";
			return false;
		}
	}
}
