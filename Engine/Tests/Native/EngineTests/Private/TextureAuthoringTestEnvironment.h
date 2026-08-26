#pragma once

#include "Asset/AssetCompilingManager.h"
#include "EngineTestSupport.h"
#include "Modules/ModuleManager.h"

#include <gtest/gtest.h>

namespace Durin::Testing
{
	class FTextureAuthoringTestEnvironment final : public testing::Environment
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

	inline testing::Environment* GTextureAuthoringTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureAuthoringTestEnvironment);
}
