#include <gtest/gtest.h>

#include "Misc/Version.h"

TEST(FEngineVersionTests, ExposesConfiguredEngineVersion)
{
	const Durin::FEngineVersion& Version = Durin::GetEngineVersion();
	EXPECT_EQ(Version.Major, DURIN_ENGINE_VERSION_MAJOR);
	EXPECT_EQ(Version.Minor, DURIN_ENGINE_VERSION_MINOR);
	EXPECT_EQ(Version.Patch, DURIN_ENGINE_VERSION_PATCH);
	EXPECT_EQ(Version.Channel, DURIN_ENGINE_VERSION_CHANNEL);
	EXPECT_EQ(Version.IsPrerelease(), !Version.Channel.empty());
	EXPECT_EQ(Durin::GetEngineVersionString(), DURIN_ENGINE_VERSION_STRING);
}
