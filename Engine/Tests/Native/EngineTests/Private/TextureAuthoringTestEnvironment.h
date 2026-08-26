#pragma once

#include "Asset/AssetCompilingManager.h"
#include "EngineTestSupport.h"
#include "Texture/Texture2DCompilingDomain.h"

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
			ASSERT_TRUE(Asset::Private::InitializeTexture2DCompilingDomain(
				GetEngineTestModuleCallbackGate()));
		}

		auto TearDown() -> void override
		{
			Asset::Private::ShutdownTexture2DCompilingDomain();
			ShutdownAssetCompilingManager();
		}
	};

	inline testing::Environment* GTextureAuthoringTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTextureAuthoringTestEnvironment);
}
