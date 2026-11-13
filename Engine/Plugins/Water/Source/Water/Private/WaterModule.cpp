#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FWaterModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FWaterModule, Water)
}
