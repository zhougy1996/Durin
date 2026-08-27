#include "EngineTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"

#include <gtest/gtest.h>

namespace
{
	// Keeps material-proxy publications admitted for the complete static-mesh test target.
	class FStaticMeshTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
			Durin::FModuleManager::Get().LoadModule("StaticMeshBuild");
			Durin::FModuleManager::Get().LoadModule("TextureBuild");
			Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
			ASSERT_EQ(
				Durin::GetRenderCommandAdmissionState(),
				Durin::ERenderCommandAdmissionState::Stopped);
			Durin::InitRenderingThread();
		}

		auto TearDown() -> void override
		{
			ASSERT_EQ(
				Durin::GetRenderCommandAdmissionState(),
				Durin::ERenderCommandAdmissionState::Running);
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
			Durin::ShutdownAssetCompilingManager();
		}
	};

	[[maybe_unused]] testing::Environment* GStaticMeshTestEnvironment =
		testing::AddGlobalTestEnvironment(new FStaticMeshTestEnvironment);
}
