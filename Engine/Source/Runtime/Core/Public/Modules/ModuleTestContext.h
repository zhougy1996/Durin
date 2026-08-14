#pragma once

#include "Modules/ModuleManager.h"

namespace Durin
{
	// Test-only lifecycle seam. Its owner generation is isolated from production
	// module-manager generations and cannot be used to mutate a loaded module.
	class FModuleTestContextFactory final
	{
	public:
		CORE_API static auto CreateStartupContext(FName ModuleName) -> FModuleContext;
		CORE_API static auto CreateShutdownContext(const FModuleContext& StartupContext) -> FModuleShutdownContext;
		CORE_API static auto InstallStartedModule(
			FName ModuleName,
			std::unique_ptr<IModuleInterface> Module
		) -> IModuleInterface*;
		CORE_API static auto SetRetirementTimeout(std::chrono::milliseconds Timeout) -> std::chrono::milliseconds;
	};
}
