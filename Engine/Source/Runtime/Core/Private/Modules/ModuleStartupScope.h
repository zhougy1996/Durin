#pragma once

#include "Misc/Name.h"

namespace Durin::Detail
{
	struct FModuleOwnerState;

	// Installs and restores the authoritative owner for one module startup callback.
	class FScopedModuleStartup final
	{
	public:
		FScopedModuleStartup(FName ModuleName, std::shared_ptr<FModuleOwnerState> ModuleOwner);
		~FScopedModuleStartup();

		FScopedModuleStartup(const FScopedModuleStartup&) = delete;
		auto operator=(const FScopedModuleStartup&) -> FScopedModuleStartup& = delete;

	private:
		std::shared_ptr<FModuleOwnerState> ModuleOwner;
	};
}
