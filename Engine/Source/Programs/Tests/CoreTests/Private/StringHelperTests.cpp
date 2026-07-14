#include "Misc/StringHelper.h"

#include <gtest/gtest.h>

namespace
{
	TEST(FStringHelperTests, FindsSubstringIgnoringCase)
	{
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("Transform Location", "location"));
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("StaticMeshComponent", "MESH"));
	}

	TEST(FStringHelperTests, HandlesEmptyAndMissingFilters)
	{
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("Actor", ""));
		EXPECT_TRUE(Durin::StringUtils::ContainsInsensitive("", ""));
		EXPECT_FALSE(Durin::StringUtils::ContainsInsensitive("Actor", "Component"));
		EXPECT_FALSE(Durin::StringUtils::ContainsInsensitive("", "Actor"));
	}
}
