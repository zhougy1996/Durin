#include "Json/Json.h"

#include <gtest/gtest.h>

namespace
{
	TEST(FJsonDocumentTests, ParseObjectFromString)
	{
		Durin::FJsonDocument Document;
		Durin::FJsonParseError Error;

		ASSERT_TRUE(Document.Parse(R"({
			"name": "yyjson smoke test",
			"version": 1,
			"flags": {
				"enabled": true,
				"threshold": 0.75
			}
		})", &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsObject());
		EXPECT_EQ(Root.GetStringValue("name"), "yyjson smoke test");
		EXPECT_EQ(Root.GetIntValue("version"), 1);

		const auto Flags = Root.GetView("flags");
		ASSERT_TRUE(Flags.IsObject());
		EXPECT_TRUE(Flags.GetBoolValue("enabled"));
		EXPECT_DOUBLE_EQ(Flags.GetDoubleValue("threshold"), 0.75);
	}

	TEST(FJsonDocumentTests, ParseArrayAndDefaults)
	{
		Durin::FJsonDocument Document;
		ASSERT_TRUE(Document.Parse(R"(["parse", 12, false])"));

		const auto Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsArray());
		ASSERT_EQ(Root.Num(), 3U);

		EXPECT_EQ(Root.GetView(0).GetString(), "parse");
		EXPECT_EQ(Root.GetView(1).GetInt(), 12);
		EXPECT_FALSE(Root.GetView(2).GetBool(true));

		EXPECT_EQ(Root.GetView(10).GetString("missing"), "missing");
		EXPECT_EQ(Root.GetView(10).GetInt(42), 42);
	}

	TEST(FJsonDocumentTests, LoadFromFile)
	{
		Durin::FJsonDocument Document;
		Durin::FJsonParseError Error;

		ASSERT_TRUE(Document.LoadFromFile(CORE_TEST_SAMPLE_JSON_FILE, &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsObject());
		EXPECT_EQ(Root.GetStringValue("name"), "yyjson smoke test");

		const auto Features = Root.GetView("features");
		ASSERT_TRUE(Features.IsArray());
		ASSERT_EQ(Features.Num(), 3U);
		EXPECT_EQ(Features.GetView(0).GetString(), "parse");
		EXPECT_EQ(Features.GetView(1).GetString(), "load-file");
		EXPECT_EQ(Features.GetView(2).GetString(), "errors");
	}

	TEST(FJsonDocumentTests, InvalidJsonReportsErrorLocation)
	{
		Durin::FJsonDocument Document;
		Durin::FJsonParseError Error;

		EXPECT_FALSE(Document.Parse("{\"broken\": [1, 2, }", &Error));
		EXPECT_NE(Error.Code, 0);
		EXPECT_FALSE(Error.Message.empty());
		EXPECT_GT(Error.BytePosition, 0U);
		EXPECT_GT(Error.Line, 0U);
		EXPECT_GT(Error.Column, 0U);
	}
}
