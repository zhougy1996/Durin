#include "RendererModule.h"
#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/UnifiedError.h"
#include "DObject/DObjectGlobals.h"
#include "Interfaces/IProjectManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/MountPaths.h"
#include "Asset/Load.h"
#include "Asset/AssetCompilingManager.h"
#include "Shader/ShaderData.h"
#include "Materials/MaterialInterface.h"
#include "Threading/TaskRuntime.h"

namespace
{
	auto Require(bool Value, std::string_view Message) -> void
	{ if (!Value) throw std::runtime_error(std::string(Message)); }
}
int main(int Argc, char** Argv)
{
	using namespace Durin;
	FRendererModule ShaderInventory;
	if (Argc < 4) return 2;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true; FNameInit();
	InitializeTaskScheduler(2);
	auto& Projects = IProjectManager::Get(); auto& Modules = FModuleManager::Get();
	try
	{
		Require(Projects.LoadProjectFile(Argv[1]) && Projects.PreparePluginStartup(), Projects.GetLastError());
		std::string Error;
		Require(FMountPaths::InitDefaultMountPoints(&Error), Error);
		auto Configuration = FAssetRuntimeConfiguration::Authored();
		Require(bool(FAssetRuntimeConfiguration::Cooked(Argv[2], Configuration)), "Invalid Cook directory.");
		Require(bool(InitializeShaderData(FShaderDataConfiguration::Cooked(Argv[2]))), "Invalid cooked Shader library.");
		Require(bool(InitializeAssetRuntime(std::move(Configuration))), "Asset runtime initialization failed.");
		DObjectInit(); InitializeAssetCompilingManager();
		Require(Projects.LoadModulesForProject(ELoadingPhase::Providers), Projects.GetLastError());
		for (int I = 3; I < Argc; ++I)
		{
			FSoftObjectPath Path;
			Require(FSoftObjectPath::TryCreate(Argv[I], Path), "Invalid cooked object path.");
			std::optional<Durin::UnifiedError::FError> LoadError;
			const auto Object = LoadObject<DObject>(Path, nullptr, &LoadError);
			Require(bool(Object), Object ? "" : LoadError ? LoadError->ToString() : "Cooked object load failed without a diagnostic.");
			if (auto* Material = Cast<DMaterialInterface>(Object))
			{
				(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
				Require(bool(Material->GetAcceptedCompiledProgram()), "Cooked material has no accepted platform program.");
			}
			std::cout << "Cooked object loaded: " << Argv[I] << '\n';
		}
		Require(!Modules.IsModuleLoaded("ShaderBuild"), "Cooked content loaded a compiler service.");
		std::cout << "Workspace content loaded in " << DURIN_RUNTIME_VARIANT << " without ShaderBuild\n";
		ShutdownAssetCompilingManager(); ShutdownAssetRuntime(); Modules.ShutdownModulesAtExit(); ShutdownTaskScheduler();
		return 0;
	}
	catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
