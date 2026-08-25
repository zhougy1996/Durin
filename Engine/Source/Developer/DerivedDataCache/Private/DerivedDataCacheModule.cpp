#include "Modules/ModuleManager.h"

namespace Durin
{
	class FDerivedDataCacheModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FDerivedDataCacheModule, DerivedDataCache)
}
