#include "Yaml/Yaml.h"

#include <gtest/gtest.h>

namespace
{
	TEST(FYamlDocumentTests, ParseObjectAndDefaults)
	{
		Durin::FYamlDocument Document;
		Durin::FYamlParseError Error;

		ASSERT_TRUE(Document.Parse(R"(name: yaml smoke test
version: 7
enabled: true
threshold: 0.75
flags:
  feature: true
  retries: 3
features:
  - parse
  - 12
  - false
)", &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsMap());
		EXPECT_TRUE(Root.Contains("flags"));
		EXPECT_EQ(Root.GetStringValue("name"), "yaml smoke test");
		EXPECT_EQ(Root.GetIntValue("version"), 7);
		EXPECT_TRUE(Root.GetBoolValue("enabled"));
		EXPECT_DOUBLE_EQ(Root.GetDoubleValue("threshold"), 0.75);
		EXPECT_EQ(Root.GetIntValue("missing", 42), 42);
		EXPECT_EQ(Root.GetDoubleValue("name", 1.25), 1.25);

		const auto Flags = Root.GetView("flags");
		ASSERT_TRUE(Flags.IsMap());
		EXPECT_EQ(Flags.GetKey(), "flags");
		EXPECT_TRUE(Flags.GetBoolValue("feature"));
		EXPECT_EQ(Flags.GetIntValue("retries"), 3);

		const auto Features = Root.GetView("features");
		ASSERT_TRUE(Features.IsSequence());
		ASSERT_EQ(Features.Num(), 3U);
		EXPECT_EQ(Features.GetView(0).GetString(), "parse");
		EXPECT_EQ(Features.GetView(1).GetInt(), 12);
		EXPECT_FALSE(Features.GetView(2).GetBool(true));
		EXPECT_EQ(Features.GetView(10).GetString("missing"), "missing");
	}

	TEST(FYamlDocumentTests, LoadFromFileAndAppConfigTemplate)
	{
		Durin::FYamlDocument Document;
		Durin::FYamlParseError Error;

		ASSERT_TRUE(Document.LoadFromFile(CORE_TEST_SAMPLE_YAML_FILE, &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsMap());
		EXPECT_EQ(Root.GetStringValue("name"), "yaml smoke test");
		EXPECT_EQ(Root.GetIntValue("version"), 7);

		Durin::FYamlDocument AppConfigDocument;
		ASSERT_TRUE(AppConfigDocument.LoadFromFile(CORE_TEST_SAMPLE_APP_CONFIG_FILE, &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto AppConfig = AppConfigDocument.GetRootView();
		EXPECT_EQ(AppConfig.GetStringValue("AppName"), "DurinApp");
		EXPECT_EQ(AppConfig.GetStringValue("LogLevel"), "Debug");
		EXPECT_FALSE(AppConfig.GetBoolValue("ForceRecompileShaders", true));
	}

	TEST(FYamlDocumentTests, BuildModifyAndRoundTrip)
	{
		Durin::FYamlDocument Document;
		Durin::FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		Root.SetStringValue("name", "writer");
		Root.SetBoolValue("enabled", true);
		Root.SetDoubleValue("threshold", 1.5);

		Durin::FYamlNodeRef Flags = Root.AddMap("flags");
		Flags.SetUIntValue("count", 3);

		Durin::FYamlNodeRef Features = Root.AddSequence("features");
		Features.AppendString("parse");
		Features.AppendInt(12);
		Features.AppendBool(false);

		Durin::FYamlNodeRef Nested = Features.AppendMap();
		Nested.SetStringValue("label", "nested");

		const std::filesystem::path OutputPath = std::filesystem::current_path() / "YamlRoundTrip.yaml";
		ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

		Durin::FYamlDocument ReloadedDocument;
		Durin::FYamlParseError Error;
		ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string(), &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto ReloadedRoot = ReloadedDocument.GetRootView();
		EXPECT_EQ(ReloadedRoot.GetStringValue("name"), "writer");
		EXPECT_TRUE(ReloadedRoot.GetBoolValue("enabled"));
		EXPECT_DOUBLE_EQ(ReloadedRoot.GetDoubleValue("threshold"), 1.5);
		EXPECT_EQ(ReloadedRoot.GetView("flags").GetUIntValue("count"), 3U);

		const auto ReloadedFeatures = ReloadedRoot.GetView("features");
		ASSERT_TRUE(ReloadedFeatures.IsSequence());
		ASSERT_EQ(ReloadedFeatures.Num(), 4U);
		EXPECT_EQ(ReloadedFeatures.GetView(0).GetString(), "parse");
		EXPECT_EQ(ReloadedFeatures.GetView(1).GetInt(), 12);
		EXPECT_FALSE(ReloadedFeatures.GetView(2).GetBool(true));
		EXPECT_EQ(ReloadedFeatures.GetView(3).GetView("label").GetString(), "nested");

		std::error_code ErrorCode;
		std::filesystem::remove(OutputPath, ErrorCode);
	}

	TEST(FYamlDocumentTests, InvalidYamlReportsLocation)
	{
		Durin::FYamlDocument Document;
		Durin::FYamlParseError Error;

		EXPECT_FALSE(Document.Parse("broken: [1, 2,", &Error));
		EXPECT_NE(Error.Code, 0);
		EXPECT_FALSE(Error.Message.empty());
		EXPECT_GT(Error.BytePosition, 0U);
		EXPECT_TRUE(Error.Line > 0U || Error.Column > 0U);
	}
}
