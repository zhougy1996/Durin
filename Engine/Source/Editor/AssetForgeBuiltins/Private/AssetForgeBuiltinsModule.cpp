#include "Modules/ModuleManager.h"
#include "AssetForgeBuiltinsAssetFeatures.h"

namespace Durin
{
	class FAssetForgeBuiltinsModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			SaveReadinessRegistration = FModuleStartup::RegisterFeature<
				IAssetSaveReadinessFeature>(AssetFeatures);
			require(SaveReadinessRegistration.IsValid());
		}

		auto ShutdownModule() -> void override
		{
			if (SaveReadinessRegistration.IsValid())
				require(SaveReadinessRegistration.Reset() == EModularFeatureRetirementStatus::Succeeded);
		}

	private:
		AssetForge::Builtins::FAssetForgeBuiltinsAssetFeatures AssetFeatures;
		FModularFeatureRegistration SaveReadinessRegistration;
	};

	IMPLEMENT_MODULE(FAssetForgeBuiltinsModule, AssetForgeBuiltins)
}
