#include "Misc/Name.h"

#include <gtest/gtest.h>

namespace
{
	TEST(FNameTests, ConstructsExplicitNumberFromCString)
	{
		const Durin::FName Name("Foo", 3);

		EXPECT_EQ(Name.ToString(), "Foo_3");
		EXPECT_EQ(Name.GetNumber(), 4U);
	}

	TEST(FNameTests, ExplicitNumberConstructorsAreEquivalent)
	{
		const Durin::FName CStringName("Foo", 3);
		const Durin::FName StringViewName(std::string_view("Foo"), 3);

		EXPECT_EQ(CStringName, StringViewName);
		EXPECT_EQ(CStringName.ToString(), StringViewName.ToString());
	}

	TEST(FNameTests, DetectedAndExplicitNumbersAreEquivalent)
	{
		const Durin::FName DetectedName("Foo_3");
		const Durin::FName ExplicitName("Foo", 3);

		EXPECT_EQ(DetectedName, ExplicitName);
		EXPECT_EQ(DetectedName.GetNumber(), ExplicitName.GetNumber());
		EXPECT_EQ(DetectedName.ToString(), ExplicitName.ToString());
	}

	TEST(FNameTests, MinusOneMeansNoNumber)
	{
		const Durin::FName Name("Foo", -1);

		EXPECT_EQ(Name.ToString(), "Foo");
		EXPECT_EQ(Name.GetNumber(), 0U);
	}

	TEST(FNameTests, ExplicitNumberComparisonIgnoresCaseByDefault)
	{
		const Durin::FName UpperName("Foo", 3);
		const Durin::FName LowerName("foo", 3);

		EXPECT_TRUE(UpperName.Equals(LowerName));
		EXPECT_FALSE(UpperName.Equals(LowerName, Durin::ENameCase::CaseSensitive));
	}
}
