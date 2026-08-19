#include "EngineTestSupport.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "AssetForgeAuthoringTestSupport.h"
#include "AssetForgeProviders.h"

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
			Durin::FModuleManager::Get().LoadModule("GeometryBuild");
			std::string Error;
			ASSERT_TRUE(Durin::Tests::InstallAssetForgeAuthoringFeatures());
			ASSERT_TRUE(Durin::Asset::Forge::RegisterAssetForgeProviders(
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
			Durin::Asset::Forge::UnregisterAssetForgeProviders();
		}
	};

	[[maybe_unused]] testing::Environment* GStaticMeshTestEnvironment =
		testing::AddGlobalTestEnvironment(new FStaticMeshTestEnvironment);
}
