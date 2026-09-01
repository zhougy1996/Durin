#include "TextureTestSupport.h"
#include "Modules/ModuleManager.h"
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
			Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
			ASSERT_TRUE(Durin::Editor::Texture::RegisterTexture2DPropertyEditing());
		}

		auto TearDown() -> void override
		{
			Durin::Editor::Texture::UnregisterTexture2DPropertyEditing();
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			const Durin::FTexture2DCompilationManagerDiagnostics BeforeShutdown =
				Durin::GetTexture2DCompilationManagerDiagnostics();
			EXPECT_EQ(BeforeShutdown.ActiveRecordCount, 0u);
			EXPECT_EQ(BeforeShutdown.QueuedWorkCount, 0u);
			EXPECT_EQ(BeforeShutdown.RunningWorkCount, 0u);
			EXPECT_EQ(BeforeShutdown.PendingCompletionCount, 0u);
			EXPECT_EQ(BeforeShutdown.InFlightEstimatedBytes, 0u);
			Durin::ShutdownAssetCompilingManager();
			const Durin::FTexture2DCompilationManagerDiagnostics AfterShutdown =
				Durin::GetTexture2DCompilationManagerDiagnostics();
			EXPECT_EQ(AfterShutdown.ActiveRecordCount, 0u);
			EXPECT_EQ(AfterShutdown.RetainedWorkCount, 0u);
			EXPECT_EQ(AfterShutdown.PendingCompletionCount, 0u);
		}
	};

	[[maybe_unused]] testing::Environment* GTextureTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureTestEnvironment);
}
