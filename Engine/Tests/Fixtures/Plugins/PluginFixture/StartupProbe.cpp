#include "Interfaces/IProjectManager.h"
#include "Project/Project.h"
#include "Modules/ModuleManager.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Class.h"
#include "Misc/Build.h"
#include "Misc/MountPaths.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include <iostream>

int main(int Argc, char** Argv)
{
	using namespace Durin;
	if (Argc != 3) return 2;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId();
	GIsGameThreadIdInitialized = true;
	FNameInit();
	auto& Projects = IProjectManager::Get();
	auto& Modules = FModuleManager::Get();
	const std::string Mode = Argv[2];
	if (Mode != "browser" && !Projects.LoadProjectFile(Argv[1])) return 3;
	if (!Projects.PreparePluginStartup())
	{
		std::cerr << Projects.GetLastError() << "\n";
		return Mode == "reject" ? 0 : 4;
	}
	if (Mode == "reject") return 5;
	if (Modules.AdmitProcessModule("UnselectedPlugin", std::filesystem::absolute("Unselected.dll").generic_string())) return 23;
	if (Modules.LoadModule("PluginFixtureEditor") || Modules.LoadModule("PluginFixtureRuntime")) return 6;
	if (Projects.LoadProjectFile(Argv[1])) return 7; // requires restart even in Browser
	if (Mode == "browser" || Mode == "disabled")
	{
		if (Modules.LoadModule("PluginFixtureConsumer")) return 19;
		Modules.ShutdownModulesAtExit();
		return 0;
	}
	std::string MountError;
	if (!FMountPaths::InitDefaultMountPoints(&MountError, true)) return 20;
	const auto MountRevision = FMountPaths::GetMountRegistryRevision();
	DObjectInit();
	const auto Load = [&](ELoadingPhase Phase) {
		if (Projects.LoadModulesForProject(Phase)) return true;
		std::cerr << Projects.GetLastError() << "\n";
		return false;
	};
	if (!Load(ELoadingPhase::Providers))
	{
		Modules.ShutdownModulesAtExit();
		return Mode == "provider-failure" ? 0 : 8;
	}
	if (Mode == "provider-failure") return 9;
	if (!Modules.IsModuleLoaded("PluginFixtureRuntime") || Modules.IsModuleLoaded("PluginFixtureConsumer")) return 10;
	if (!FindClassByQualifiedName("Durin::PluginFixture::DFixtureAsset")) return 11;
	std::string Error;
	if (!Projects.SetPluginEnabled("PluginFixtureConsumer", false, Error)) return 12;
	if (Mode == "save-disable" && !Projects.SaveCurrentProjectToDisk(Error)) return 21;
	if (FMountPaths::GetMountRegistryRevision() != MountRevision) return 22;
	if (!Load(ELoadingPhase::EngineReady)) return 13;
	if (!Modules.IsModuleLoaded("PluginFixtureConsumer")
		|| !FindClassByQualifiedName("Durin::PluginFixture::DConsumerAsset")) return 14;
	if (Modules.UnloadModule("PluginFixtureRuntime")) return 15;
#if DURIN_WITH_EDITOR
	if (!Load(ELoadingPhase::EditorReady) || !Modules.IsModuleLoaded("PluginFixtureEditor")) return 16;
#else
	if (Modules.LoadModule("PluginFixtureEditor")) return 17;
#endif
	Modules.ShutdownModulesAtExit();
	if (Modules.IsModuleLoaded("PluginFixtureRuntime") || Modules.LoadModule("PluginFixtureRuntime")) return 18;
	return 0;
}
