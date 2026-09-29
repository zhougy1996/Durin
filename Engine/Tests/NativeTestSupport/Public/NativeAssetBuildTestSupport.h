#pragma once

#include "Asset/AssetCompilingManager.h"
#include "Modules/ModuleManager.h"
#include <gtest/gtest.h>

#if DURIN_WITH_EDITOR
namespace Durin::Testing
{
	// Opt-in process-root module setup for native fixtures that execute authored builds.
	inline auto LoadAssetBuildModulesForTests() -> void
	{
		FModuleManager::Get().LoadModuleChecked("TextureBuild");
		FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	}
	class FAssetBuildTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override { LoadAssetBuildModulesForTests(); }
		auto TearDown() -> void override
		{
			ShutdownAssetCompilingManager();
		}
	};
	inline testing::Environment* GAssetBuildTestEnvironment =
		testing::AddGlobalTestEnvironment(new FAssetBuildTestEnvironment);
}
#endif
