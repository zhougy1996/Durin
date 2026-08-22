#include "TextureTestSupport.h"
#include "AssetForgeAuthoringTestSupport.h"
#include "AssetForgeProviders.h"
#include "Texture2DPropertyEditing.h"
#include "TextureSourceRelocation.h"

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
			ASSERT_TRUE(Durin::Tests::InstallAssetForgeAuthoringFeatures());
			ASSERT_TRUE(Durin::Asset::Forge::RegisterAssetForgeProviders(
				Error, GetEngineTestModuleCallbackGate())) << Error;
			ASSERT_TRUE(Durin::Editor::Texture::RegisterTexture2DPropertyEditing());
			ASSERT_TRUE(Durin::Editor::Texture::RegisterTextureSourceRelocation());
		}

		auto TearDown() -> void override
		{
			Durin::Editor::Texture::UnregisterTextureSourceRelocation();
			Durin::Editor::Texture::UnregisterTexture2DPropertyEditing();
			Durin::Asset::Forge::UnregisterAssetForgeProviders();
			Durin::Asset::Build::ShutdownBuildHost();
			Durin::Asset::Build::ShutdownTextureBuildService();
		}
	};

	[[maybe_unused]] testing::Environment* GTextureTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureTestEnvironment);
}
