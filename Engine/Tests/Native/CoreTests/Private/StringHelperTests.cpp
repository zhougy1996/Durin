#include "Misc/StringHelper.h"
#include "Misc/StringConvert.h"

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

	TEST(FStringHelperTests, ConvertsUnicodeBetweenWideAndUtf8)
	{
		constexpr std::string_view Utf8 = "Durin-\xe4\xb8\xad\xe6\x96\x87-\xf0\x9f\x8d\x8e";
		const std::wstring Wide = Durin::StringUtils::Utf8ToWide(Utf8);
		EXPECT_FALSE(Wide.empty());
		EXPECT_EQ(Durin::StringUtils::WideToUtf8(Wide), Utf8);
	}
}
