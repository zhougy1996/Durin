#include "TextureTestSupport.h"
#include "StandardAssetAuthoringTestSupport.h"
#include "StandardAssetImportProviders.h"

namespace
{
	// TextureTests is an authoring-process root: it selects and drains the host
	// explicitly so production submission never falls back to lazy ownership.
	class FTextureTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(EnsureTextureBuildHost());
			std::string Error;
			ASSERT_TRUE(Durin::Tests::InstallStandardAssetAuthoringFeatures());
			ASSERT_TRUE(Durin::Asset::Import::Standard::RegisterStandardAssetImportProviders(
				Error, GetEngineTestModuleCallbackGate())) << Error;
		}

		auto TearDown() -> void override
		{
			Durin::Asset::Import::Standard::UnregisterStandardAssetImportProviders();
			Durin::Asset::Build::ShutdownBuildHost();
			Durin::Asset::Build::ShutdownTextureBuildService();
		}
	};

	[[maybe_unused]] testing::Environment* GTextureTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureTestEnvironment);
}
