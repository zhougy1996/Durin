#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

#include "Modules/ModuleManager.h"

namespace Durin
{
	// Drives a concrete stack-owned module through isolated startup and shutdown in tests.
	class FModuleTestHarness final
	{
	public:
		CORE_API explicit FModuleTestHarness(FName ModuleName);
		CORE_API ~FModuleTestHarness();

		FModuleTestHarness(const FModuleTestHarness&) = delete;
		auto operator=(const FModuleTestHarness&) -> FModuleTestHarness& = delete;

		// Starts exactly one module and requires Shutdown before harness destruction.
		CORE_API auto Start(IModuleInterface& Module) -> void;
		CORE_API auto Shutdown() -> void;

		CORE_API static auto InstallStartedModule(
			FName ModuleName,
			std::unique_ptr<IModuleInterface> Module
		) -> IModuleInterface*;

	private:
		FName ModuleName;
		std::shared_ptr<Detail::FModuleOwnerState> ModuleOwner;
		IModuleInterface* StartedModule = nullptr;
	};
}
