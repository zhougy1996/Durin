#include "Plugins/PluginManagement.h"
#include "Interfaces/IProjectManager.h"
#include "Project/Project.h"
#include "Modules/ModuleManager.h"
#include "Misc/MountPaths.h"
#include "Misc/FileHelper.h"
#include "DObject/DObjectGlobals.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "HAL/PlatformProcess.h"
#include <iostream>

int main(int Argc, char** Argv)
{
    using namespace Durin;
    if (Argc != 3) return 2;
    GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true; FNameInit();
    const std::string Mode = Argv[2]; const std::filesystem::path File = Argv[1];
    auto& Projects = IProjectManager::Get(); auto& Modules = FModuleManager::Get();
    if (!Projects.LoadProjectFile(File.generic_string()) || !Projects.PreparePluginStartup())
    { std::cerr << Projects.GetLastError(); return 3; }
    std::string Error; if (!FMountPaths::InitDefaultMountPoints(&Error)) return 4;
    if (Mode == "ownership-parent")
    {
        if (!AcquireProjectEditOwnership()) return 20;
        const auto Result = FPlatformProcess::ExecuteProcess(Argv[0],
            FProcessLaunchOptions{.Arguments = {File.generic_string(), "ownership"}});
        ReleaseProjectEditOwnership();
        return Result.value_or(21);
    }
    const auto Revision = FMountPaths::GetMountRegistryRevision();
    FPluginManagementContext Context;
    if (!MakePluginManagementContext(Context, Error)) return 5;
    const auto Active = Context.ActivePlugins;
    const auto Original = FFileHelper::LoadFileToArray(File);
    if (!Original) return 6;
    const auto Restore = [&] { return FFileHelper::SaveArrayToFile(*Original, File); };
    FPluginEditSession Session;
    if (!Session.Open(Context, Error)) return 7;
    if (Mode == "enable")
    {
        if (!Session.SetEnabled("PluginFixtureRuntime",true,Error)
            || !Session.SetEnabled("PluginFixtureConsumer",true,Error)) return 8;
    }
    else if (Mode == "save-blocked")
    {
        if (!Session.SetEnabled("PluginFixtureRuntime",false,Error)) return 22;
        const auto Consumer = std::ranges::find(Session.GetRows(),"PluginFixtureConsumer",[](const auto& Row){return Row.Info.Id;});
        if (Consumer == Session.GetRows().end() || !Consumer->bRequested || Consumer->bNextStart) return 23;
    }
    else if (Mode != "observe-enabled" && Mode != "observe-disabled" && Mode != "observe-blocked")
    {
        if (!Session.SetEnabled("PluginFixtureConsumer",false,Error)
            || !Session.SetEnabled("PluginFixtureRuntime",false,Error)) return 9;
    }
    if (Mode == "readonly")
    {
        const auto Permissions = std::filesystem::status(File).permissions();
        std::filesystem::permissions(File, std::filesystem::perms::owner_read);
        const bool Saved = Session.Save(Error);
        std::filesystem::permissions(File, Permissions);
        if (Saved || !Session.IsDirty() || *FFileHelper::LoadFileToArray(File) != *Original) return 10;
    }
    else if (Mode == "stale")
    {
        { std::ofstream Stream(File,std::ios::app); Stream << " "; }
        const bool Saved = Session.Save(Error);
        if (!Restore() || Saved || !Session.IsDirty() || Error.find("changed") == std::string::npos) return 11;
    }
    else if (Mode == "ownership")
    {
        if (Session.Save(Error) || !Session.IsDirty() || *FFileHelper::LoadFileToArray(File) != *Original) return 12;
    }
    else if (Mode == "missing-build")
    {
        const auto Backup = Context.InventoryFile.parent_path()/"Inventory.held";
        std::filesystem::rename(Context.InventoryFile,Backup);
        const bool Saved = Session.Save(Error);
        const bool Ready = Session.CheckRestartReady(Error);
        std::filesystem::rename(Backup,Context.InventoryFile);
        if (!Saved || Ready || Session.IsDirty() || Error.find("configure/build") == std::string::npos) return 13;
    }
    else if (Mode == "disable" || Mode == "enable" || Mode == "save-blocked")
    {
        if (!Session.Save(Error) || Session.IsDirty() || !Session.RequiresRestart()
            || !Session.CheckRestartReady(Error)) { std::cerr << Error; return 14; }
    }
    if (GetActivePluginIds().size()!=Active.size() || !std::ranges::equal(GetActivePluginIds(),Active)
        || FMountPaths::GetMountRegistryRevision()!=Revision) return 15;
    const bool bConsumer = std::ranges::find(Active,"PluginFixtureConsumer")!=Active.end();
    if ((Mode=="observe-enabled" && !bConsumer) || ((Mode=="observe-disabled" || Mode=="observe-blocked") && bConsumer)) return 16;
    if (Mode=="observe-blocked")
    {
        const auto Consumer = std::ranges::find(Session.GetRows(),"PluginFixtureConsumer",[](const auto& Row){return Row.Info.Id;});
        if (Consumer==Session.GetRows().end() || !Consumer->bRequested || Consumer->bNextStart || Session.IsDirty()) return 24;
    }
    DObjectInit();
    for (const auto Phase : {ELoadingPhase::Providers,ELoadingPhase::EngineReady,ELoadingPhase::EditorReady})
        if (!Projects.LoadModulesForProject(Phase)) { std::cerr << Projects.GetLastError(); return 17; }
    if (Modules.IsModuleLoaded("PluginFixtureConsumer")!=bConsumer) return 18;
    if (!bConsumer && Modules.LoadModule("PluginFixtureConsumer")) return 19;
    Modules.ShutdownModulesAtExit();
    std::cout << Mode << ": passed; running=" << Active.size() << "; " << Error << "\n";
    return 0;
}
