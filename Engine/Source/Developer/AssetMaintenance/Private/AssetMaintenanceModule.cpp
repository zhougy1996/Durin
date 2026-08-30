#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetMaintenanceModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FAssetMaintenanceModule, AssetMaintenance)
}
