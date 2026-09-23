#include "Modules/ModuleTestSupport.h"

#include "Modules/ModuleStartupScope.h"

namespace Durin
{
	FModuleTestOwner::FModuleTestOwner(FName InModuleName)
		: ModuleName(std::move(InModuleName))
	{
		static std::atomic<uint64> NextTestGeneration = 1;
		const uint64 Generation = NextTestGeneration.fetch_add(1, std::memory_order_relaxed);
		ModuleOwner = FModularFeatureRegistry::Get().CreateOwner(ModuleName, Generation);
	}

	auto FModuleTestOwner::GetFeatureSnapshot() const
		-> FModularFeatureRetirementSnapshot
	{
		return FModularFeatureRegistry::Get().SnapshotOwner(ModuleOwner);
	}

	FModuleTestHarness::FModuleTestHarness(FName ModuleName)
		: Owner(std::move(ModuleName))
	{
	}

	FModuleTestHarness::~FModuleTestHarness()
	{
		check(StartedModule == nullptr);
	}

	auto FModuleTestHarness::Start(IModuleInterface& Module) -> void
	{
		require(StartedModule == nullptr);
		Detail::FScopedModuleStartup StartupScope(Owner.ModuleName, Owner.ModuleOwner);
		Module.StartupModule();
		StartedModule = &Module;
	}

	auto FModuleTestHarness::Shutdown() -> void
	{
		require(StartedModule != nullptr);
		StartedModule->ShutdownModule();
		const auto Audit = Owner.GetFeatureSnapshot();
		require(Audit.PublishedCount == 0);
		require(Audit.InFlightInvocationCount == 0);
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
		ModuleInfo->ModuleOwner = FModularFeatureRegistry::Get().CreateOwner(
			ModuleName, ModuleInfo->OwnerGeneration);
		ModuleInfo->Module = std::move(Module);
		Detail::FScopedModuleStartup StartupScope(ModuleName, ModuleInfo->ModuleOwner);
		ModuleInfo->Module->StartupModule();
		ModuleInfo->LoadOrder = Manager.NextLoadOrder++;
		ModuleInfo->State = EModuleState::Active;
		return ModuleInfo->Module.get();
	}
}
