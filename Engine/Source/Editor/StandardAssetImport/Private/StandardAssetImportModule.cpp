#include "Modules/ModuleManager.h"
#include "StandardAssetImportProviders.h"

namespace Durin
{
	class FStandardAssetImportModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			std::string Error;
			checkf(RegisterStandardAssetImportProviders(Error), "{}", Error);
		}

		auto ShutdownModule() -> void override
		{
			UnregisterStandardAssetImportProviders();
		}
	};

	IMPLEMENT_MODULE(FStandardAssetImportModule, StandardAssetImport)
}
