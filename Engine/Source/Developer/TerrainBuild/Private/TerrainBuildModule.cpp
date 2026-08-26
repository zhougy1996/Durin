#include "TerrainBuildFunctionRegistry.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	// Owns Terrain build-function registration for the loaded module generation.
	class FTerrainBuildModule final : public IModuleInterface
	{
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;

		auto StartupModule() -> void override
		{
			std::string Error;
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("TerrainBuild.BuildFunctions");
			checkf(Asset::InitializeTerrainBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"TerrainBuild could not register its build functions: {}", Error);
		}

		auto ShutdownModule() -> void override
		{
			Asset::ShutdownTerrainBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FTerrainBuildModule, TerrainBuild)
}
