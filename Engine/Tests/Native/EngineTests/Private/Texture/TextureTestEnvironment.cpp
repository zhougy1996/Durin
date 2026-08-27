#include "TextureTestSupport.h"
#include "AssetForgeBuiltinsAssetTestSupport.h"
#include "Texture2DPropertyEditing.h"

namespace
{
	// TextureTests is an editor-process root for concrete post-load features.
	class FTextureTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(EnsureTextureCompilingManager());
			ASSERT_TRUE(Durin::Tests::InstallAssetForgeBuiltinsAssetFeatures());
			ASSERT_TRUE(Durin::Editor::Texture::RegisterTexture2DPropertyEditing());
		}

		auto TearDown() -> void override
		{
			Durin::Editor::Texture::UnregisterTexture2DPropertyEditing();
			Durin::ShutdownAssetCompilingManager();
		}
	};

	[[maybe_unused]] testing::Environment* GTextureTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureTestEnvironment);
}
