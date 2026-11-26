#include "TextureTestSupport.h"
#include "Modules/ModuleManager.h"

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
			Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
		}

		auto TearDown() -> void override
		{
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			EXPECT_EQ(Durin::FAssetCompilingManager::Get().GetNumRemainingAssets(), 0u);
			Durin::ShutdownAssetCompilingManager();
		}
	};

	[[maybe_unused]] testing::Environment* GTextureTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureTestEnvironment);
}
