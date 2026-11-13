#include "CoreMinimal.h"
#include "Interfaces/IProjectManager.h"
#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Class.h"

int main(int Argc, char** Argv)
{
	using namespace Durin;
	if (Argc != 3) return 2;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId();
	GIsGameThreadIdInitialized = true;
	FNameInit();
	const bool Enabled = std::string_view(Argv[2]) == "enabled";
	if (!Enabled && std::string_view(Argv[2]) != "disabled") return 3;
	auto& Projects = IProjectManager::Get();
	auto& Modules = FModuleManager::Get();
	if (!Projects.LoadProjectFile(Argv[1]) || !Projects.PreparePluginStartup())
	{
		std::cerr << Projects.GetLastError() << '\n';
		return 4;
	}
	// Frozen selection must reject early plugin lookup, even with a DLL present.
	if (Modules.LoadModule("Water")) return 5;
	DObjectInit();
	if (!Projects.LoadModulesForProject(ELoadingPhase::Providers))
	{
		std::cerr << Projects.GetLastError() << '\n';
		return 6;
	}
	const bool Loaded = Modules.LoadModule("Water") != nullptr;
	const bool HasActor = FindClassByQualifiedName("Durin::AWaterSurfaceActor") != nullptr;
	Modules.ShutdownModulesAtExit();
	if (Loaded != Enabled || HasActor != Enabled) return 7;
	std::cout << "Water " << (Loaded ? "enabled" : "disabled") << '\n';
	return 0;
}
