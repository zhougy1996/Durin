#include "EngineTestSupport.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "AssetForgeBuiltinsAssetTestSupport.h"
#include "AssetForgeBuiltinsProviders.h"

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
			Durin::FModuleManager::Get().LoadModule("StaticMeshBuild");
			std::string Error;
			ASSERT_TRUE(Durin::Tests::InstallAssetForgeBuiltinsAssetFeatures());
			ASSERT_TRUE(Durin::AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
				Error, GetEngineTestModuleCallbackGate())) << Error;
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
			Durin::AssetForge::Builtins::UnregisterAssetForgeBuiltinsProviders();
		}
	};

	[[maybe_unused]] testing::Environment* GStaticMeshTestEnvironment =
		testing::AddGlobalTestEnvironment(new FStaticMeshTestEnvironment);
}
