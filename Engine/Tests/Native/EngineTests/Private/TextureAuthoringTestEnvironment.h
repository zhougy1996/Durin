#pragma once

#include "AssetBuild/BuildHost.h"
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
			ASSERT_TRUE(Asset::Build::InitializeTextureBuildService(
				GetEngineTestModuleCallbackGate()));
			ASSERT_TRUE(Asset::Build::InitializeBuildHost());
		}

		auto TearDown() -> void override
		{
			Asset::Build::ShutdownBuildHost();
			Asset::Build::ShutdownTextureBuildService();
		}
	};

	inline testing::Environment* GTextureAuthoringTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureAuthoringTestEnvironment);
}
