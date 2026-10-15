#include "Modules/ModuleTestSupport.h"

#include "Modules/ModuleStartupScope.h"
#include "Modules/ModuleOwnerState.h"

namespace Durin
{
	FModuleTestHarness::FModuleTestHarness(FName InModuleName)
		: ModuleName(InModuleName), ModuleOwner(std::make_shared<Detail::FModuleOwnerState>())
	{
		ModuleOwner->Name = InModuleName;
	}

	FModuleTestHarness::~FModuleTestHarness()
	{
		check(StartedModule == nullptr);
	}

	auto FModuleTestHarness::Start(IModuleInterface& Module) -> void
	{
		require(StartedModule == nullptr);
		Detail::FScopedModuleStartup StartupScope(ModuleName, ModuleOwner);
		Module.StartupModule();
		StartedModule = &Module;
	}

	auto FModuleTestHarness::Shutdown() -> void
	{
		require(StartedModule != nullptr);
		StartedModule->ShutdownModule();
		require(!FModularFeatureRegistry::Get().IsDispatching());
		StartedModule = nullptr;
	}

	auto FModuleTestHarness::InstallStartedModule(
		FName ModuleName,
		std::unique_ptr<IModuleInterface> Module) -> IModuleInterface*
	{
		if (!Module) return nullptr;
		auto& Manager = FModuleManager::Get();
		Manager.AddModule(ModuleName, "<core-test-module>");
		auto ModuleInfo = Manager.FindModule(ModuleName);
		if (!ModuleInfo) return nullptr;
		const EModuleState State = ModuleInfo->State.load();
		if (State != EModuleState::Registered && State != EModuleState::Unloaded
			&& State != EModuleState::LoadFailed)
		{
			return nullptr;
		}
		ModuleInfo->State = EModuleState::Loading;
		ModuleInfo->OwnerGeneration = Manager.NextOwnerGeneration++;
		ModuleInfo->ModuleOwner = std::make_shared<Detail::FModuleOwnerState>(Detail::FModuleOwnerState{ModuleName, ModuleInfo->OwnerGeneration});
		ModuleInfo->Module = std::move(Module);
		Detail::FScopedModuleStartup StartupScope(ModuleName, ModuleInfo->ModuleOwner);
		ModuleInfo->Module->StartupModule();
		ModuleInfo->LoadOrder = Manager.NextLoadOrder++;
		ModuleInfo->State = EModuleState::Active;
		return ModuleInfo->Module.get();
	}
}
