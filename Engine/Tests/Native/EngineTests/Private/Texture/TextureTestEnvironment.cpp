#include "TextureTestSupport.h"
#include "AssetForgeBuiltinsAuthoringTestSupport.h"
#include "AssetForgeBuiltinsProviders.h"
#include "Texture2DPropertyEditing.h"
#include "TextureSourceRelocation.h"

namespace
{
	// TextureTests is an authoring-process root: it selects the optional provider
	// explicitly so production submission never falls back to lazy ownership.
	class FTextureTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(EnsureTextureCompilingManager());
			std::string Error;
			ASSERT_TRUE(Durin::Tests::InstallAssetForgeBuiltinsAuthoringFeatures());
			ASSERT_TRUE(Durin::AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
				Error, GetEngineTestModuleCallbackGate())) << Error;
			ASSERT_TRUE(Durin::Editor::Texture::RegisterTexture2DPropertyEditing());
			ASSERT_TRUE(Durin::Editor::Texture::RegisterTextureSourceRelocation());
		}

		auto TearDown() -> void override
		{
			Durin::Editor::Texture::UnregisterTextureSourceRelocation();
			Durin::Editor::Texture::UnregisterTexture2DPropertyEditing();
			Durin::AssetForge::Builtins::UnregisterAssetForgeBuiltinsProviders();
			Durin::Asset::Private::ShutdownTexture2DCompilationDomain();
			Durin::ShutdownAssetCompilingManager();
		}
	};

	[[maybe_unused]] testing::Environment* GTextureTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureTestEnvironment);
}
