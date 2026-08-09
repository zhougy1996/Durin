#include "Misc/StringHelper.h"

#include <gtest/gtest.h>

namespace
{
	TEST(FStringHelperTests, CaseInsensitiveFilteringHandlesMatchesAndEmptyInputs)
	{
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("Transform Location", "location"));
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("StaticMeshComponent", "MESH"));
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("Actor", ""));
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("", ""));
		EXPECT_FALSE(Durin::StringUtils::ContainsInsensitive("Actor", "Component"));
		EXPECT_FALSE(Durin::StringUtils::ContainsInsensitive("", "Actor"));
	}

	TEST(FStringHelperTests, HumanizesCamelCaseAndPreservesAcronyms)
	{
		EXPECT_EQ(Durin::StringUtils::HumanizeName("GroundHeight"), "Ground Height");
		EXPECT_EQ(Durin::StringUtils::HumanizeName("URLValue"), "URL Value");
		EXPECT_EQ(Durin::StringUtils::HumanizeName("HDR"), "HDR");
		EXPECT_EQ(Durin::StringUtils::HumanizeName("border"), "border");
	}
}
