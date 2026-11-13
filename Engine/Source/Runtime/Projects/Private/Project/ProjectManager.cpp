#include "Interfaces/IProjectManager.h"
#include "Project/Project.h"
#include "ProjectContextInternal.h"
#include "Modules/ModuleManager.h"
#include "Misc/StringHelper.h"
#include "PluginStartup.h"
#include "Plugins/PluginResources.h"
#include "Plugins/PluginManagement.h"
#include "Misc/FileHelper.h"
#include "Misc/AppConfig.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"
#include "Json/Json.h"

namespace Durin
{
	namespace
	{
		FPluginResources ActivePluginResources;
        std::vector<std::string> ActivePluginIds;
        std::vector<std::string> StartupDiagnostics;
		class FProjectManager final : public IProjectManager
		{
			std::optional<FProjectDescriptor> Descriptor;
			std::optional<FProjectInfo> Active;
			mutable std::string LastError;
			bool bDirty = false;
			bool bModulesStarted = false;
			bool bPluginAdmissionFrozen = false;
			bool bPluginStartupFailed = false;
            bool bAllowPluginFailures = false;
            bool bProjectModulesAvailable = true;
            std::map<std::string, std::vector<std::string>> PluginDependencies;
            std::set<std::string> FailedPlugins;
            auto DisablePlugin(const std::string& Id, const std::string& Reason) -> void
            {
                const auto Key = StringUtils::FoldAscii(Id);
                if (!FailedPlugins.insert(Key).second) return;
                StartupDiagnostics.push_back("Plugin " + Id + " could not start: " + Reason);
                DURIN_WARN("{}", StartupDiagnostics.back());
                std::erase_if(ActivePluginIds, [&](const auto& Name) { return StringUtils::FoldAscii(Name) == Key; });
                for (const auto& [Consumer, Dependencies] : PluginDependencies)
                    if (std::ranges::any_of(Dependencies, [&](const auto& Name) { return StringUtils::FoldAscii(Name) == Key; }))
                        DisablePlugin(Consumer, "required plugin " + Id + " failed");
                if (Descriptor && std::ranges::any_of(Descriptor->PluginDependencies, [&](const auto& Name) { return StringUtils::FoldAscii(Name) == Key; }))
                    bProjectModulesAvailable = false;
            }
			int CompletedPhase = -1;
			std::vector<FAdmittedPluginBinary> PluginBinaries;
            std::vector<std::string> PluginDiagnostics;
		public:
			auto GetLastError() const -> const std::string& override { return LastError; }
			auto Fail(std::string Message,std::string* OutReason=nullptr) const -> bool
			{ LastError=std::move(Message); if (OutReason) *OutReason=LastError; return false; }
			auto LoadProjectFile(std::string_view InFile) -> bool override
			{
				if (bModulesStarted || bPluginAdmissionFrozen) return Fail("Changing the active project requires restart");
				FProjectDescriptor Candidate;
				if (!Candidate.Load(InFile,LastError)) return false;
				auto Committed=CommitProjectContext(InFile,Candidate);
				if (!Committed) return Fail(Committed.error());
				Active=std::move(*Committed);
				Descriptor = std::move(Candidate);
				bDirty = false;
				LastError.clear(); return true;
			}
			auto GetActiveProject() const -> const FProjectInfo* override { return Active ? &*Active : nullptr; }
			auto SetHistoryRecording(bool Enabled) -> void { if (Active) Active->bRecordInteractiveHistory=Enabled; }
			auto GetCurrentProject() const -> const FProjectDescriptor* override { return Descriptor ? &*Descriptor : nullptr; }
			auto ClearCurrentProject() -> void override { if (bPluginAdmissionFrozen) { Fail("Changing the active project requires restart"); return; } ClearProjectContext(); Descriptor.reset(); Active.reset(); bDirty=false; bModulesStarted=false; CompletedPhase=-1; bPluginStartupFailed=false; }
			auto PreparePluginStartup(bool bAllowUnavailablePlugins) -> bool override
			{
				if (bPluginAdmissionFrozen) return !bPluginStartupFailed;
                bAllowPluginFailures = bAllowUnavailablePlugins;
                bProjectModulesAvailable = true;
                StartupDiagnostics.clear(); FailedPlugins.clear(); PluginDependencies.clear();
				FPluginResolution Selection;
				std::vector<FAdmittedPluginBinary> Binaries;
				FPluginResources Resources;
                if (!ResolveProcessPlugins(Active ? Active->ProjectFile : "", Selection, Binaries, Resources, LastError, bAllowUnavailablePlugins, StartupDiagnostics)) return false;
                auto Additional = Descriptor ? Descriptor->Mounts : std::vector<FMountPoint>{};
                Additional.insert(Additional.end(), Resources.ContentMounts.begin(), Resources.ContentMounts.end());
                if (Active && !FMountPaths::ValidateMountDefinitions(BuildProjectMountDefinitions(Active->ProjectFile, Additional), &LastError)) return false;
				std::vector<FName> Ordinary;
				for (const auto& Name : Selection.OrdinaryModules) Ordinary.emplace_back(Name);
				std::vector<std::pair<FName, std::string>> ProcessModules;
				for (const auto& Binary : Binaries)
				{
					const auto Utf8 = Binary.Path.generic_u8string();
					ProcessModules.emplace_back(FName(Binary.Module.Name),
						std::string(reinterpret_cast<const char*>(Utf8.data()), Utf8.size()));
				}
				if (!FModuleManager::Get().FreezeModuleLoading(Ordinary, ProcessModules)) return Fail("Cannot freeze module admission; restart required");
                if (Active) FMountPaths::SetProjectMountDefinitions(std::move(Additional));
                ActivePluginResources = std::move(Resources);
                ActivePluginIds = Selection.Plugins;
                for (const auto& Id : Selection.Plugins)
                {
                    FJsonDocument Metadata;
                    if (const auto It = Selection.Locations.find("Plugin/" + Id); It != Selection.Locations.end() && Metadata.LoadFromFile(It->second))
                    {
                        const auto Dependencies = Metadata.GetRootView().GetView("Dependencies");
                        for (size_t I = 0; I < Dependencies.Num(); ++I) PluginDependencies[Id].push_back(Dependencies.GetView(I).GetString());
                    }
                }
                if (Descriptor) for (const auto& Id : Descriptor->PluginDependencies)
                    if (std::ranges::none_of(ActivePluginIds, [&](const auto& Name) { return StringUtils::FoldAscii(Name) == StringUtils::FoldAscii(Id); }))
                        bProjectModulesAvailable = false;
                if (!bProjectModulesAvailable)
                    StartupDiagnostics.push_back("Project modules and content were not initialized because a project-required plugin is unavailable");
                FJsonDocument Manifest;
                if (Manifest.Parse(Selection.ManifestJson))
                {
                    const auto Inactive=Manifest.GetRootView().GetView("InactivePlugins");
                    for (size_t I=0;I<Inactive.Num();++I)
                        PluginDiagnostics.push_back("Plugin "+Inactive.GetView(I).GetView("Id").GetString()+" remains inactive: "+Inactive.GetView(I).GetView("Reason").GetString());
                }
				PluginBinaries = std::move(Binaries);
				bPluginAdmissionFrozen = true;
				LastError.clear();
				return true;
			}
			auto LoadModulesForProject(ELoadingPhase Phase,const std::function<bool()>& IsCanceled) -> bool override
			{
				if (bPluginStartupFailed) return false;
				const auto Index = static_cast<int>(Phase);
				if (Index < 0 || Index >= 3) return Fail("Invalid project module loading phase");
				if (Index <= CompletedPhase) { LastError.clear(); return true; }
				if (Index != CompletedPhase + 1) return Fail("Project module phases must start in order");
				const auto* Context=Durin::GetCurrentProject();
				const auto FailStartup = [&](std::string Message) { bPluginStartupFailed=true; return Fail(std::move(Message)); };
				const std::array<std::string_view, 3> Phases{"Providers", "EngineReady", "EditorReady"};
                // Hosts have initialized logging by Providers. Admission itself
                // remains quiet for machine-readable offline tool reports.
                if (Phase==ELoadingPhase::Providers)
                    {
                        for (const auto& Diagnostic:PluginDiagnostics) DURIN_WARN("{}",Diagnostic);
                        for (const auto& Diagnostic:StartupDiagnostics) DURIN_WARN("{}",Diagnostic);
                    }
				for (const auto& Binary : PluginBinaries)
				{
					if (Binary.Module.Phase != Phases[Index] || FailedPlugins.contains(StringUtils::FoldAscii(Binary.Module.Owner))) continue;
					if (IsCanceled && IsCanceled()) return FailStartup("Plugin startup canceled");
					const auto Utf8 = Binary.Path.generic_u8string();
					const std::string Path(reinterpret_cast<const char*>(Utf8.data()), Utf8.size());
					if (!FModuleManager::Get().AdmitProcessModule(FName(Binary.Module.Name), Path)
						|| !FModuleManager::Get().LoadModule(FName(Binary.Module.Name)))
                    {
                        if (!bAllowPluginFailures) return FailStartup("Plugin module could not start: " + Binary.Module.Owner + "/" + Binary.Module.Name);
                        DisablePlugin(Binary.Module.Owner, "module " + Binary.Module.Name + " failed");
                    }
				}
				if (Phase == ELoadingPhase::Providers && Context && bProjectModulesAvailable)
					for (const auto& Name : Context->EnabledRootModules)
					{
						if (IsCanceled && IsCanceled()) return FailStartup("Project module startup canceled");
						bModulesStarted=true;
                        if (!FModuleManager::Get().LoadModule(FName(Name)))
                        {
                            if (!bAllowPluginFailures) return FailStartup("Project module could not start: " + Name);
                            bProjectModulesAvailable = false;
                            StartupDiagnostics.push_back("Project module " + Name + " could not start; project content was not initialized");
                            DURIN_WARN("{}", StartupDiagnostics.back());
                            break;
                        }
					}
				bModulesStarted = Context != nullptr || bPluginAdmissionFrozen;
				CompletedPhase=Index;
				LastError.clear(); return true;
			}
			auto QueryStatusForProject(std::string_view InFile,FProjectStatus& OutStatus) const -> bool override
			{
				FProjectDescriptor Candidate;
				if (!Candidate.Load(InFile,LastError)) return false;
				OutStatus={Candidate.Name,NormalizeProjectFile(InFile)}; LastError.clear(); return true;
			}
			auto QueryStatusForCurrentProject(FProjectStatus& OutStatus) const -> bool override
			{
				if (!Descriptor) return Fail("No project loaded");
				OutStatus={Descriptor->Name,Active->ProjectFile}; LastError.clear(); return true;
			}
			auto AreProjectModulesAvailable() const -> bool override { return bProjectModulesAvailable; }
			auto IsCurrentProjectDirty() const -> bool override { return bDirty; }
			auto SaveCurrentProjectToDisk(std::string& OutReason) -> bool override
			{
				if (!Descriptor) return Fail("No project loaded",&OutReason);
				if (auto Ownership = AcquireProjectEditOwnership(); !Ownership) return Fail(Ownership.error().ToString(),&OutReason);
				if (!Descriptor->Save(Active->ProjectFile,LastError)) return Fail(LastError,&OutReason);
				bDirty = false;
				LastError.clear(); OutReason.clear(); return true;
			}
            auto SavePluginOverrides(const std::map<std::string, bool>& Overrides, std::string_view ExpectedDisk, std::string_view ExpectedProjectDisk, std::string& OutReason) -> bool
            {
                if (!Descriptor || !Active) return Fail("No project loaded", &OutReason);
                if (bDirty) return Fail("Save the existing project descriptor edits before changing plugins", &OutReason);
                if (auto Ownership = AcquireProjectEditOwnership(); !Ownership) return Fail(Ownership.error().ToString(), &OutReason);
                const auto Bytes = FFileHelper::LoadFileToArray(Active->ProjectFile);
                if (!Bytes || std::string_view(reinterpret_cast<const char*>(Bytes->data()), Bytes->size()) != ExpectedProjectDisk)
                    return Fail("Project descriptor changed on disk; refresh plugin management before saving", &OutReason);
                FProjectDescriptor Candidate;
                if (!Candidate.Load(Active->ProjectFile, LastError)) return Fail(LastError, &OutReason);
                const auto StartupDisk = ReadPluginStartupSnapshot(Active->ProjectFile);
                if (!StartupDisk || *StartupDisk != ExpectedDisk)
                    return Fail("Plugin startup configuration changed on disk; refresh before saving", &OutReason);
                FPluginManagementContext Context;
                if (!MakePluginManagementContext(Context, LastError)) return Fail(LastError, &OutReason);
                Context.Request.PluginOverrides = Overrides;
                const auto Selection = ResolvePlugins(Context.Request);
                if (!Selection) return Fail(Selection.error(), &OutReason);
                FPluginResources Resources;
                if (!ResolvePluginResources(*Selection, Resources, LastError)) return Fail(LastError, &OutReason);
                auto Mounts = Candidate.Mounts;
                Mounts.insert(Mounts.end(), Resources.ContentMounts.begin(), Resources.ContentMounts.end());
                if (!FMountPaths::ValidateMountDefinitions(BuildProjectMountDefinitions(Active->ProjectFile, Mounts), &LastError)) return Fail(LastError, &OutReason);
                FJsonDocument Output; auto Root = Output.GetMutableRoot(); Root.EnsureObject();
                auto Plugins = Root.AddObject("Plugins");
                for (const auto& [Id, Enabled] : Overrides) Plugins.SetChildValue(Id, Enabled);
                const auto File = PluginStartupConfigFile(Active->ProjectFile);
                std::error_code Error; std::filesystem::create_directories(File.parent_path(), Error);
                if (Error) return Fail("Cannot create plugin startup configuration directory: " + Error.message(), &OutReason);
                const auto Text = Output.ToString();
                if (auto Saved = FFileHelper::SaveArrayToFileAtomically(std::as_bytes(std::span(Text.data(), Text.size())), File); !Saved)
                    return Fail(Saved.error().ToString(), &OutReason);
                LastError.clear(); OutReason.clear(); return true;
            }

		};
	}

	namespace { auto Manager() -> FProjectManager& { static FProjectManager Instance; return Instance; } }
    auto GetActivePluginIds() -> std::span<const std::string> { return ActivePluginIds; }
    auto GetPluginStartupDiagnostics() -> std::span<const std::string> { return StartupDiagnostics; }
    auto SaveProjectPluginOverrides(const std::map<std::string, bool>& Overrides, std::string_view ExpectedDisk, std::string_view ExpectedProjectDisk, std::string& OutReason) -> bool
    { return Manager().SavePluginOverrides(Overrides, ExpectedDisk, ExpectedProjectDisk, OutReason); }

    auto GetActivePluginResources() -> const FPluginResources& { return ActivePluginResources; }
    auto LoadProjectConfig(const std::filesystem::path& File, FYamlDocument& OutDocument, std::string& OutReason) -> bool
    {
        try
        {
            std::vector<std::filesystem::path> Files;
            const auto* Project = GetCurrentProject();
            if (Project && std::filesystem::weakly_canonical(File) == std::filesystem::weakly_canonical(std::filesystem::path(Project->ProjectDir) / "Configs/Project.yaml"))
            {
                Files.emplace_back(std::filesystem::path(FPaths::EngineDir()) / "Configs/Project.yaml");
                for (const auto& Directory : ActivePluginResources.ConfigDirectories) Files.push_back(Directory / "Project.yaml");
            }
            Files.push_back(File);
            return LoadConfigLayers(Files, OutDocument, OutReason);
        }
        catch (const std::exception& Error) { OutReason = Error.what(); return false; }
    }

	auto IProjectManager::Get() -> IProjectManager& { return Manager(); }
	auto InitializeCurrentProject(const FProjectInitializationParams& Params) -> std::expected<void,FProjectError>
	{
		auto& Service=Manager();
		if (Params.bOpenProjectBrowser || Params.RequestedProjectFile.empty()) { Service.ClearCurrentProject(); return {}; }
		if (!Service.LoadProjectFile(Params.RequestedProjectFile)) return std::unexpected(FProjectError{EProjectError::InvalidDescriptor,Service.GetLastError()});
		Service.SetHistoryRecording(Params.bRecordInteractiveHistory); return {};
	}
}
