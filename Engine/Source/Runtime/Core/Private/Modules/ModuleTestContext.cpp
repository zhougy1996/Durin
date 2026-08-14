#include "Modules/ModuleTestContext.h"

namespace Durin
{
	auto FModuleTestContextFactory::CreateStartupContext(FName ModuleName) -> FModuleContext
	{
		static std::atomic<uint64> NextTestGeneration = 1;
		const uint64 Generation = NextTestGeneration.fetch_add(1, std::memory_order_relaxed);
		auto Owner = FModularFeatureRegistry::Get().CreateOwner(ModuleName, Generation);
		return FModuleContext(ModuleName, std::move(Owner));
	}

	auto FModuleTestContextFactory::CreateShutdownContext(const FModuleContext& StartupContext) -> FModuleShutdownContext
	{
		auto Retirement = FModularFeatureRegistry::Get().RetireOwner(StartupContext.Owner, std::chrono::seconds(5));
		Detail::BeginRetireAsyncOperationOwner(StartupContext.Owner);
		return FModuleShutdownContext(
			StartupContext.ModuleName, std::move(Retirement.Snapshot), StartupContext.Owner);
	}

	auto FModuleTestContextFactory::InstallStartedModule(
		FName ModuleName,
		std::unique_ptr<IModuleInterface> Module
	) -> IModuleInterface*
	{
		if (!Module) return nullptr;
		auto& Manager = FModuleManager::Get();
		Manager.AddModule(ModuleName, "<core-test-module>");
		auto ModuleInfo = Manager.FindModule(ModuleName);
		if (!ModuleInfo) return nullptr;
		const EModuleState State = ModuleInfo->State.load();
		if (State != EModuleState::Registered && State != EModuleState::Unloaded && State != EModuleState::LoadFailed)
		{
			return nullptr;
		}
		ModuleInfo->State = EModuleState::Loading;
		ModuleInfo->OwnerGeneration = Manager.NextOwnerGeneration++;
		ModuleInfo->FeatureOwner = FModularFeatureRegistry::Get().CreateOwner(ModuleName, ModuleInfo->OwnerGeneration);
		ModuleInfo->Module = std::move(Module);
		FModuleContext Context(ModuleName, ModuleInfo->FeatureOwner);
		ModuleInfo->Module->StartupModule(Context);
		ModuleInfo->State = EModuleState::Active;
		return ModuleInfo->Module.get();
	}

	auto FModuleTestContextFactory::SetRetirementTimeout(std::chrono::milliseconds Timeout) -> std::chrono::milliseconds
	{
		auto& Manager = FModuleManager::Get();
		return std::exchange(Manager.FeatureRetirementTimeout, Timeout);
	}
}
