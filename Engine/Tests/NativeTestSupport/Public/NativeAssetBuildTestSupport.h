#pragma once

#include "Asset/AssetBuildService.h"
#include "Asset/AssetCompilingManager.h"
#include "Modules/ModuleManager.h"
#include <gtest/gtest.h>

#if DURIN_WITH_EDITOR
namespace Durin::Testing
{
	// Opt-in process-root ownership for native fixtures that execute authored builds.
	inline auto InitializeAssetBuildServiceForTests() -> bool
	{
		FModuleManager::Get().LoadModuleChecked("TextureBuild");
		FModuleManager::Get().LoadModuleChecked("MeshBuilder");
		return InitializeAssetBuildService();
	}
	class FAssetBuildTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override { ASSERT_TRUE(InitializeAssetBuildServiceForTests()); }
		auto TearDown() -> void override
		{
			ShutdownAssetCompilingManager();
			ShutdownAssetBuildService();
		}
	};
	inline testing::Environment* GAssetBuildTestEnvironment =
		testing::AddGlobalTestEnvironment(new FAssetBuildTestEnvironment);
}
#endif
