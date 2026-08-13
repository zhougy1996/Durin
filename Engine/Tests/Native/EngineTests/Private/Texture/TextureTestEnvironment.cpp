#include "TextureTestSupport.h"
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
			ASSERT_TRUE(Durin::Asset::Import::RegisterStandardAssetImportProviders(Error)) << Error;
		}

		auto TearDown() -> void override
		{
			Durin::Asset::Import::UnregisterStandardAssetImportProviders();
			Durin::Asset::Build::ShutdownBuildHost();
			Durin::Asset::Build::ShutdownTextureBuildService();
		}
	};

	[[maybe_unused]] testing::Environment* GTextureTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureTestEnvironment);
}
