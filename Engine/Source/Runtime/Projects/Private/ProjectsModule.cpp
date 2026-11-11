#include "Modules/ModuleManager.h"

namespace Durin
{
	class FProjectsModule final : public IModuleInterface {};
	IMPLEMENT_MODULE(FProjectsModule, Projects)
}
