#include "Modules/ModuleManager.h"

namespace Durin
{
	class FDurinMCPModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FDurinMCPModule, DurinMCP)
}
