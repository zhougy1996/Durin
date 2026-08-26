#pragma once

#include "Asset/AssetCompilingManager.h"
#include "EngineTestSupport.h"
#include "Modules/ModuleManager.h"

#include <gtest/gtest.h>

namespace Durin::Testing
{
	class FTextureAssetTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(InitializeAssetCompilingManager());
			FModuleManager::Get().LoadModuleChecked("TextureBuild");
		}

		auto TearDown() -> void override
		{
			ShutdownAssetCompilingManager();
		}
	};

	inline testing::Environment* GTextureAssetTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureAssetTestEnvironment);
}
