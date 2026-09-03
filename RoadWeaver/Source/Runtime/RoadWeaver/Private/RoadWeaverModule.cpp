#include "Modules/ModuleManager.h"

namespace Durin
{
	class FRoadWeaverModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FRoadWeaverModule, RoadWeaver)
}
