#include "Yaml/Yaml.h"
#include "Misc/AppConfig.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeYamlTestPath(std::string_view FileName) -> std::filesystem::path
	{
		return Durin::Testing::GetTestWorkDirectory() / std::string(FileName);
	}

	auto MakeYamlTestDataPath(std::string_view FileName) -> std::filesystem::path
	{
		return std::filesystem::path{DURIN_TEST_DATA_DIR} / std::string(FileName);
	}

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
		std::string Name;
		int64 Version = 0;
		bool bEnabled = false;
		double Threshold = 0.0;
		EXPECT_TRUE(Root.GetChildValue("name", Name));
		EXPECT_TRUE(Root.GetChildValue("version", Version));
		EXPECT_TRUE(Root.GetChildValue("enabled", bEnabled));
		EXPECT_TRUE(Root.GetChildValue("threshold", Threshold));
		EXPECT_EQ(Name, "yaml smoke test");
		EXPECT_EQ(Version, 7);
		EXPECT_TRUE(bEnabled);
		EXPECT_DOUBLE_EQ(Threshold, 0.75);
		EXPECT_EQ(Root.GetView("missing").GetInt(42), 42);
		EXPECT_EQ(Root.GetView("name").GetDouble(1.25), 1.25);

		const auto Flags = Root.GetView("flags");
		ASSERT_TRUE(Flags.IsMap());
		EXPECT_EQ(Flags.GetKey(), "flags");
		EXPECT_TRUE(Flags.GetView("feature").GetBool());
		EXPECT_EQ(Flags.GetView("retries").GetInt(), 3);

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

		ASSERT_TRUE(Document.LoadFromFile(MakeYamlTestDataPath("Sample.yaml").string(), &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsMap());
		EXPECT_EQ(Root.GetView("name").GetString(), "yaml smoke test");
		EXPECT_EQ(Root.GetView("version").GetInt(), 7);

		Durin::FYamlDocument AppConfigDocument;
		ASSERT_TRUE(AppConfigDocument.LoadFromFile(MakeYamlTestDataPath("TP_DurinEditor.yaml").string(), &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto AppConfig = AppConfigDocument.GetRootView();
		const auto Logging = AppConfig.GetView("Core").GetView("Logging");
		EXPECT_EQ(Logging.GetView("ConsoleLevel").GetString(), "Debug");

		const auto GC = AppConfig.GetView("CoreDObject").GetView("GC");
		EXPECT_TRUE(GC.GetView("Enabled").GetBool(false));
		EXPECT_DOUBLE_EQ(GC.GetView("IntervalSeconds").GetDouble(), 60.0);
		EXPECT_DOUBLE_EQ(GC.GetView("MaxIntervalSeconds").GetDouble(), 600.0);
		EXPECT_DOUBLE_EQ(GC.GetView("IntervalBackoffMultiplier").GetDouble(), 2.0);
		EXPECT_EQ(GC.GetView("PendingKillThreshold").GetUInt(), 128U);
		EXPECT_EQ(GC.GetView("ObjectGrowthThreshold").GetUInt(), 1024U);

		EXPECT_FALSE(AppConfig.Contains("AppName"));
		EXPECT_FALSE(AppConfig.Contains("LogLevel"));
		EXPECT_FALSE(AppConfig.Contains("ForceRecompileShaders"));
		EXPECT_FALSE(AppConfig.Contains("GarbageCollection"));
	}

	TEST(FYamlDocumentTests, ModuleConfigViews)
	{
		EXPECT_FALSE(Durin::GetModuleConfig("Core").IsValid());

		ASSERT_TRUE(Durin::LoadAppConfig(MakeYamlTestDataPath("TP_DurinEditor.yaml").string()));
		const Durin::FYamlNodeView CoreConfig = Durin::GetModuleConfig("Core");
		ASSERT_TRUE(CoreConfig.IsMap());
		EXPECT_EQ(CoreConfig.GetView("Logging").GetView("ConsoleLevel").GetString(), "Debug");
		EXPECT_FALSE(Durin::GetModuleConfig("MissingModule").IsValid());
	}

	TEST(FYamlDocumentTests, BuildModifyAndRoundTrip)
	{
		Durin::FYamlDocument Document;
		Durin::FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		Root.SetChildValue("name", "writer");
		Root.SetChildValue("enabled", true);
		Root.SetChildValue("threshold", 1.5);

		Durin::FYamlNodeRef Flags = Root.AddMap("flags");
		Flags.SetChildValue("count", static_cast<uint64>(3));

		Durin::FYamlNodeRef Features = Root.AddSequence("features");
		Features.AppendValue("parse");
		Features.AppendValue(static_cast<int64>(12));
		Features.AppendValue(false);
		EXPECT_EQ(Features.GetView(0).GetString(), "parse");

		Durin::FYamlNodeRef Nested = Features.AppendMap();
		Nested.SetChildValue("label", "nested");
		EXPECT_EQ(Root.GetRef("flags").GetView("count").GetUInt(), 3U);
		EXPECT_EQ(Features.GetRef(3).GetView("label").GetString(), "nested");

		const std::filesystem::path OutputPath = MakeYamlTestPath("YamlRoundTrip.yaml");
		ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

		Durin::FYamlDocument ReloadedDocument;
		Durin::FYamlParseError Error;
		ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string(), &Error));
		EXPECT_EQ(Error.Code, 0);

		const auto ReloadedRoot = ReloadedDocument.GetRootView();
		EXPECT_EQ(ReloadedRoot.GetView("name").GetString(), "writer");
		EXPECT_TRUE(ReloadedRoot.GetView("enabled").GetBool());
		EXPECT_DOUBLE_EQ(ReloadedRoot.GetView("threshold").GetDouble(), 1.5);
		EXPECT_EQ(ReloadedRoot.GetView("flags").GetView("count").GetUInt(), 3U);

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

	TEST(FYamlDocumentTests, ModifyParsedScalarsAndRoundTrip)
	{
		Durin::FYamlDocument Document;
		Durin::FYamlParseError Error;
		ASSERT_TRUE(Document.Parse(R"(Editor:
  DefaultLevel: /Game/Levels/TestLevel
  Enabled: false
)", &Error));

		Durin::FYamlNodeRef Editor = Document.GetMutableRoot().GetRef("Editor");
		ASSERT_TRUE(Editor.IsMap());
		Editor.SetChildValue("DefaultLevel", "/Game/Levels/NewLevel");
		Editor.SetChildValue("Enabled", true);

		const std::filesystem::path OutputPath = MakeYamlTestPath("YamlModifiedRoundTrip.yaml");
		ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

		Durin::FYamlDocument ReloadedDocument;
		ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string(), &Error));
		const Durin::FYamlNodeView ReloadedEditor = ReloadedDocument.GetRootView().GetView("Editor");
		EXPECT_EQ(ReloadedEditor.GetView("DefaultLevel").GetString(), "/Game/Levels/NewLevel");
		EXPECT_TRUE(ReloadedEditor.GetView("Enabled").GetBool());

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
