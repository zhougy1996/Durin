#include "TargetPlatform.h"
#include <gtest/gtest.h>

using namespace Durin;

TEST(FTargetPlatformTests, StableIdentitiesRejectUnknownValues)
{
	static_assert(uint32(ECookTargetPlatform::Win64) == 1 && uint32(ECookTargetPlatform::MacOS) == 2);
	static_assert(uint32(EAssetPayloadTargetPlatform::Win64) == 1 && uint32(EShaderTargetPlatform::MacOS) == 2);
	static_assert(uint32(ECookTargetProfile::Game) == 1 && uint32(EShaderTargetProfile::EditorValidation) == 2);
	for (const auto Platform : {ECookTargetPlatform::Win64, ECookTargetPlatform::MacOS})
	{
		EXPECT_EQ(ParseTargetPlatform(GetTargetPlatformName(Platform)), Platform);
		const auto Shader = ToShaderTargetPlatform(Platform);
		ASSERT_TRUE(Shader);
		EXPECT_EQ(ToCookTargetPlatform(*Shader), Platform);
	}
	EXPECT_EQ(ParseTargetPlatform("macos"), ECookTargetPlatform::MacOS);
	EXPECT_EQ(ParseTargetProfile("game"), ECookTargetProfile::Game);
	EXPECT_FALSE(ParseTargetPlatform("linux"));
	EXPECT_FALSE(ParseTargetProfile("server"));
	EXPECT_TRUE(GetTargetPlatformName(static_cast<ECookTargetPlatform>(99)).empty());
	EXPECT_FALSE(ToShaderTargetPlatform(static_cast<ECookTargetPlatform>(99)));
	EXPECT_FALSE(ToCookTargetPlatform(static_cast<EShaderTargetPlatform>(99)));
	EXPECT_FALSE(ToShaderTargetProfile(static_cast<ECookTargetProfile>(99)));
}

TEST(FTargetPlatformTests, ResolutionExposesPlatformCapabilities)
{
	const auto Mac = ResolveTargetPlatform(ECookTargetPlatform::MacOS);
	ASSERT_TRUE(Mac);
	EXPECT_EQ(Mac->GetShaderTarget(), MetalShaderTarget);
	EXPECT_FALSE(Mac->SupportsCollisionPayload());
	const auto Windows = ResolveTargetPlatform(ECookTargetPlatform::Win64);
	ASSERT_TRUE(Windows);
	EXPECT_EQ(Windows->GetShaderTarget(), VulkanShaderTarget);
	EXPECT_TRUE(Windows->SupportsCollisionPayload());
	EXPECT_FALSE(ResolveTargetPlatform(ECookTargetPlatform::Invalid));
}
