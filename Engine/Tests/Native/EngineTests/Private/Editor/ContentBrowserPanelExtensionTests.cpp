#include "Panels/ContentBrowserPanel.h"
#include "EngineTestSupport.h"
#include "NativeTestSupport.h"
#include "imgui_internal.h"

#include <gtest/gtest.h>

namespace Durin::Editor::ContentBrowser::Private
{
	// Exercises production UI entrypoints without creating an OS window or starting a renderer.
	struct FContentBrowserPanelTestAccess
	{
		static auto SetSelection(FContentBrowserPanel& Panel, const std::string& Root,
			std::vector<FContentBrowserItem> Items) -> void
		{
			Panel.Model.SetSnapshotForTesting(Root, std::move(Items));
			Panel.Selection.clear();
			for (const auto& Item : Panel.Model.GetItems()) Panel.Selection.insert(Item.StableId());
			Panel.bShowSelectionDetails = true;
			Panel.ViewMode = EContentBrowserViewMode::Details;
		}
		static auto DrawContent(FContentBrowserPanel& Panel) -> void { Panel.DrawContentArea(); }
		static auto DrawItemMenu(FContentBrowserPanel& Panel) -> void
		{
			Panel.DrawItemContextMenu(Panel.Model.GetItems().front());
		}
	};

	TEST(FContentBrowserPanelExtensionTests, DrawsRegisteredDetailsAndContextMenuInRealPanel)
	{
		InitializeDObjectSystem();
		const std::string Root = Testing::CreateTestFixtureDirectory("BrowserPanelExtensions").generic_string();
		ImGuiContext* UI = ImGui::CreateContext();
		struct FContextCleanup
		{
			ImGuiContext* Context;
			~FContextCleanup() { ImGui::DestroyContext(Context); }
		} Cleanup{UI};
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1200, 900);
		IO.DeltaTime = 1.0f / 60.0f;
		unsigned char* Pixels;
		int Width, Height;
		IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);

		FContentBrowserPanel Panel({}, {}, {}, {}, {}, {}, {}, {}, {},
			std::make_shared<FMountedContentReconciliationState>(), {});
		std::string Error;
		int TypeDetailCalls = 0;
		auto Type = RegisterAssetTypePresentation({
			.AssetClassName = "PanelTest::DNovelAsset", .DisplayName = "Novel asset", .Icon = "*",
			.Details = [&](const auto&) {
				++TypeDetailCalls;
				return std::vector<FDetailRow>{{"Custom property", "Type detail rendered"}};
			}}, Error);
		ASSERT_TRUE(Type.IsValid()) << Error;
		auto Details = RegisterExtension({
			.Id = "panel-test.details", .Label = "Selection", .Category = EExtensionCategory::Details,
			.Mutation = EContentMutation::ReadOnly,
			.IsApplicable = [](const auto& Context) { return !Context.Selection.empty(); },
			.Details = [](const auto& Context) {
				return std::vector<FDetailRow>{{"Extension property", Context.Selection.size() == 1
					? "Single detail rendered" : "Multiple details rendered"}};
			}}, Error);
		ASSERT_TRUE(Details.IsValid()) << Error;
		auto Menu = RegisterExtension({
			.Id = "panel-test.context", .Label = "Custom context rendered", .Category = EExtensionCategory::ContextMenu,
			.Mutation = EContentMutation::ReadOnly,
			.IsApplicable = [](const auto& Context) { return Context.PrimaryItem.has_value(); },
			.Invoke = [](const auto&) {}}, Error);
		ASSERT_TRUE(Menu.IsValid()) << Error;

		const FContentBrowserItem Asset{.Kind = EContentBrowserItemKind::Asset, .Name = "Novel",
			.VirtualPath = "/PanelTest/Novel", .PhysicalPath = Root + "/Novel.dasset",
			.AssetClassName = "PanelTest::DNovelAsset"};
		FContentBrowserPanelTestAccess::SetSelection(Panel, Root, {Asset});
		auto Draw = [&](bool bMenu) {
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(1100, 800));
			ImGui::Begin("Browser extension test");
			ImGui::LogToBuffer();
			if (bMenu) FContentBrowserPanelTestAccess::DrawItemMenu(Panel);
			else FContentBrowserPanelTestAccess::DrawContent(Panel);
			const std::string Text(UI->LogBuffer.c_str());
			ImGui::LogFinish();
			ImGui::End();
			ImGui::EndFrame();
			return Text;
		};
		const std::string Single = Draw(false);
		EXPECT_NE(Single.find("Type detail rendered"), std::string::npos) << Single;
		EXPECT_NE(Single.find("Single detail rendered"), std::string::npos) << Single;
		const std::string MenuText = Draw(true);
		EXPECT_NE(MenuText.find("Custom context rendered"), std::string::npos) << MenuText;
		FContentBrowserPanelTestAccess::SetSelection(Panel, Root, {Asset,
			{.Kind = EContentBrowserItemKind::File, .Name = "Source", .PhysicalPath = Root + "/source.txt"}});
		const int SingleCalls = TypeDetailCalls;
		const std::string Multiple = Draw(false);
		EXPECT_NE(Multiple.find("Multiple details rendered"), std::string::npos) << Multiple;
		EXPECT_EQ(TypeDetailCalls, SingleCalls);
		Menu.Reset();
		Details.Reset();
		Type.Reset();
		EXPECT_EQ(Draw(true).find("Custom context rendered"), std::string::npos);
		EXPECT_EQ(Draw(false).find("Multiple details rendered"), std::string::npos);
		Panel.StopRequestAdmission();
	}
}
