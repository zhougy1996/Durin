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
			requiref(Asset::Import::Standard::RegisterStandardAssetImportProviders(Error), "{}", Error);
		}

		auto ShutdownModule() -> void override
		{
			Asset::Import::Standard::UnregisterStandardAssetImportProviders();
		}
	};

	IMPLEMENT_MODULE(FStandardAssetImportModule, StandardAssetImport)
}
