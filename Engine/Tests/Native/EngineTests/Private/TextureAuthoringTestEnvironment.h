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
			ASSERT_TRUE(AssetBuild::InitializeTextureBuildService());
			ASSERT_TRUE(AssetBuild::InitializeBuildHost());
		}

		auto TearDown() -> void override
		{
			AssetBuild::ShutdownBuildHost();
			AssetBuild::ShutdownTextureBuildService();
		}
	};

	inline testing::Environment* GTextureAuthoringTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureAuthoringTestEnvironment);
}
