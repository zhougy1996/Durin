#include "Plugins/PluginResolution.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Project.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	TEST(PluginResolutionTests, SharedPortableSelectionsAndFailures)
	{
		Durin::FJsonDocument Cases;
		ASSERT_TRUE(Cases.LoadFromFile(std::filesystem::path(DURIN_TEST_DATA_DIR) / "Cases.json"));
		const auto List = Cases.GetRootView().GetView("Cases");
		for (size_t I = 0; I < List.Num(); ++I)
		{
			const auto Case = List.GetView(I);
			const auto Name = Case.GetView("Name").GetString();
			SCOPED_TRACE(Name);
			const auto Root = Durin::Testing::CreateTestFixtureDirectory(Name);
			Case.GetView("Files").ForEachObjectMember([&](std::string_view Relative, Durin::FJsonNodeView Text) {
				const std::string Raw = Text.GetString();
				ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
					Durin::FByteView(reinterpret_cast<const std::byte*>(Raw.data()), Raw.size()),
					Root / std::string(Relative)));
			});
			Durin::FPluginResolveRequest Request;
			Request.EngineRoot = Root / "Engine";
			Request.ProjectFile = Root / "Game/Game.dproject";
			Request.RuntimeVariant = Case.GetView("Variant").GetString("DurinEditor");
			Request.Platform = Case.GetView("Platform").GetString("Win64");
			const auto Result = Durin::ResolvePlugins(Request);
			if (Case.Contains("Error"))
			{
				ASSERT_FALSE(Result);
				EXPECT_NE(Result.error().find(Case.GetView("Error").GetString()), std::string::npos) << Result.error();
				if (Case.Contains("Chain")) EXPECT_NE(Result.error().find(Case.GetView("Chain").GetString()), std::string::npos) << Result.error();
			}
			else
			{
				ASSERT_TRUE(Result) << Result.error();
				EXPECT_EQ(Result->ManifestJson, Case.GetView("Manifest").GetString());
				EXPECT_EQ(Result->ManifestJson.find(Root.generic_string()), std::string::npos);
				for (const auto& [Logical, Path] : Result->Locations) EXPECT_TRUE(Path.is_absolute());
			}
		}
	}

	TEST(PluginResolutionTests, ProjectOverridesDoNotAlterModuleRoots)
	{
		Durin::FJsonDocument Document;
		ASSERT_TRUE(Document.Parse(R"({"Plugins":{"Water":true,"Tools":false}})"));
		auto Overrides = Durin::ReadProjectPluginOverrides(Document.GetRootView());
		ASSERT_TRUE(Overrides);
		EXPECT_EQ(Overrides->size(), 2);
		EXPECT_TRUE(Overrides->at("Water"));
		EXPECT_FALSE(Overrides->at("Tools"));
		ASSERT_TRUE(Document.Parse(R"({"Plugins":{"Water":true,"water":false}})"));
		EXPECT_FALSE(Durin::ReadProjectPluginOverrides(Document.GetRootView()));
		ASSERT_TRUE(Document.Parse(R"({"Plugins":[]})"));
		EXPECT_FALSE(Durin::ReadProjectPluginOverrides(Document.GetRootView()));
	}
}
