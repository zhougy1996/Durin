#pragma once

#include "Asset/AssetCompilingManager.h"
#include "EngineTestSupport.h"
#include "Texture/TextureBuildService.h"

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
			ASSERT_TRUE(Asset::Build::InitializeTextureBuildService(
				GetEngineTestModuleCallbackGate()));
		}

		auto TearDown() -> void override
		{
			Asset::Build::ShutdownTextureBuildService();
			ShutdownAssetCompilingManager();
		}
	};

	inline testing::Environment* GTextureAuthoringTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureAuthoringTestEnvironment);
}
