#include "Panels/ContentBrowserPanel.h"
#include "EngineTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "Materials/Material.h"
#include "Import/EditorReimportHandler.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "imgui_internal.h"
#include "Asset/AssetDragDrop.h"

#include <gtest/gtest.h>
#include <fstream>

namespace Durin::Editor::ContentBrowser::Private
{
	// Exercises production UI entrypoints without creating an OS window or starting a renderer.
	struct FContentBrowserPanelTestAccess
	{
		static auto Copy(FContentBrowserPanel& Panel) -> void { Panel.CopyContentSelection(); }
		static auto Paste(FContentBrowserPanel& Panel, std::string_view Folder) -> void { Panel.PasteContent(Folder); }
		static auto CanPaste(const FContentBrowserPanel& Panel) -> bool { return Panel.HasContentClipboard(); }
		static auto CreateFolder(FContentBrowserPanel& Panel, const std::string& Root) -> void
		{
			Panel.CreateFolder(Root);
		}
		static auto FinishCapture(FContentBrowserPanel& Panel) -> void
		{
			Panel.Model.WaitForPendingSnapshotsForTesting();
			Panel.RepairSelection();
			Panel.CompletePendingItemAction();
		}
		static auto RenameTarget(const FContentBrowserPanel& Panel) -> std::string { return Panel.RenameTarget; }
		static auto IsSelected(const FContentBrowserPanel& Panel, const std::string& Path) -> bool
		{
			return Panel.SelectionState.Selected.contains(Path);
		}
		static auto Navigate(FContentBrowserPanel& Panel, const std::string& Directory) -> bool
		{
			return Panel.NavigateToPhysical(Directory);
		}
		static auto SetSelection(FContentBrowserPanel& Panel, const std::string& Root,
			std::vector<FContentBrowserItem> Items) -> void
		{
			Panel.Model.SetSnapshotForTesting(Root, std::move(Items));
			Panel.SelectionState.Selected.clear();
			for (const auto& Item : Panel.Model.GetItems()) Panel.SelectionState.Selected.insert(Item.StableId());
			Panel.bShowSelectionDetails = true;
			Panel.ViewMode = EContentBrowserViewMode::Details;
		}
		static auto DrawContent(FContentBrowserPanel& Panel) -> void { Panel.DrawContentArea(); }
		static auto BeginDrag(FContentBrowserPanel& Panel) -> void
		{ Panel.BeginAssetDragDrop(Panel.Model.GetItems().front()); }
		static auto SetReimportQuery(FContentBrowserPanel& Panel, FContentBrowserPanel::FQueryReimport Query) -> void
		{ Panel.QueryReimport = std::move(Query); }
		static auto DrawItemMenu(FContentBrowserPanel& Panel) -> void
		{
			Panel.DrawItemContextMenu(Panel.Model.GetItems().front());
		}
	};

	TEST(FContentBrowserPanelExtensionTests, CopiesOrdinarySelectionAcrossPanelsAndInvalidatesReplacedClipboard)
	{
		InitializeDObjectSystem();
		const auto Root = Testing::CreateTestFixtureDirectory("BrowserContentClipboard");
		std::filesystem::create_directories(Root / "Destination");
		std::ofstream(Root / "notes.txt") << "notes";
		std::ofstream(Root / "config.json") << "{}";
		const std::array Definitions{FMountPoint{.VirtualRoot = "/BrowserContentClipboard/", .Owner = EMountOwner::Test,
			.Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid());
		ImGuiContext* UI = ImGui::CreateContext();
		struct FContextCleanup { ImGuiContext* Context; ~FContextCleanup() { ImGui::DestroyContext(Context); } } Cleanup{UI};
		// Keep the test clipboard in memory instead of changing the user's clipboard.
		std::string Clipboard;
		auto& Platform = ImGui::GetPlatformIO();
		Platform.Platform_ClipboardUserData = &Clipboard;
		Platform.Platform_GetClipboardTextFn = [](ImGuiContext* Context) -> const char* {
			return static_cast<std::string*>(Context->PlatformIO.Platform_ClipboardUserData)->c_str();
		};
		Platform.Platform_SetClipboardTextFn = [](ImGuiContext* Context, const char* Text) {
			*static_cast<std::string*>(Context->PlatformIO.Platform_ClipboardUserData) = Text;
		};
		FContentBrowserPanel Source({}, {}, {}, {}, {}, {}, {}, {}, std::make_shared<FMountedContentReconciliationState>(), {});
		FContentBrowserPanel Target({}, {}, {}, {}, {}, {}, {}, {}, std::make_shared<FMountedContentReconciliationState>(), {});
		FContentBrowserPanelTestAccess::SetSelection(Source, Root.generic_string(), {
			{.Kind = EContentBrowserItemKind::File, .Name = "notes.txt", .PhysicalPath = (Root / "notes.txt").generic_string()},
			{.Kind = EContentBrowserItemKind::File, .Name = "config.json", .PhysicalPath = (Root / "config.json").generic_string()}});
		FContentBrowserPanelTestAccess::Copy(Source);
		ASSERT_TRUE(FContentBrowserPanelTestAccess::CanPaste(Target));
		FContentBrowserPanelTestAccess::Paste(Target, "/BrowserContentClipboard/Destination/");
		EXPECT_TRUE(std::filesystem::exists(Root / "Destination/notes.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Destination/config.json"));
		FContentBrowserPanelTestAccess::Paste(Target, "/BrowserContentClipboard/Destination/");
		EXPECT_TRUE(std::filesystem::exists(Root / "Destination/notes_Copy.txt"));
		ImGui::SetClipboardText("unrelated text");
		EXPECT_FALSE(FContentBrowserPanelTestAccess::CanPaste(Target));
	}

	TEST(FContentBrowserPanelExtensionTests, DeferredFolderRenameWaitsForCaptureAndIsCanceledByNavigation)
	{
		InitializeDObjectSystem();
		const auto Root = Testing::CreateTestFixtureDirectory("BrowserDeferredRename");
		std::filesystem::create_directory(Root / "Other");
		const std::array Definitions{FMountPoint{
			.VirtualRoot = "/BrowserDeferredRename/", .Owner = EMountOwner::Test,
			.Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid());
		FContentBrowserPanel Panel({}, {}, {}, {}, {}, {}, {}, {},
			std::make_shared<FMountedContentReconciliationState>(), {});
		FContentBrowserPanelTestAccess::CreateFolder(Panel, Root.generic_string());
		EXPECT_TRUE(FContentBrowserPanelTestAccess::RenameTarget(Panel).empty());
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		const auto Created = (Root / "New Folder").generic_string();
		EXPECT_EQ(FContentBrowserPanelTestAccess::RenameTarget(Panel), Created);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::IsSelected(Panel, Created));
		FContentBrowserPanelTestAccess::CreateFolder(Panel, Root.generic_string());
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Panel, (Root / "Other").generic_string()));
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		EXPECT_NE(FContentBrowserPanelTestAccess::RenameTarget(Panel), (Root / "New Folder (2)").generic_string());
		EXPECT_FALSE(FContentBrowserPanelTestAccess::IsSelected(Panel, (Root / "New Folder (2)").generic_string()));
	}

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

		FContentBrowserPanel Panel({}, {}, {}, {}, {}, {}, {}, {},
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

namespace Durin::Editor::ContentBrowser::Private
{
	TEST(FContentBrowserPanelExtensionTests, SelectionMigratesOrderedRenamesAndRemovesDeletedSubtrees)
	{
		const auto Root = Testing::GetTestWorkDirectory();
		const auto A = (Root / "A").generic_string();
		const auto B = (Root / "B").generic_string();
		const auto C = (Root / "C").generic_string();
		FContentBrowserSelection State;
		State.Selected = {"/Game/A/Asset.Asset", A + "/file.txt", "/Game/AB/Keep.Keep",
			"/Game/a/Case.Case", "/Game/A.Package"};
		State.Anchor = "/Game/A/Asset.Asset";
		State.Apply({.Changes = {
			{EContentChangeKind::Renamed, A, B, "/Game/A/", "/Game/B/", true},
			{EContentChangeKind::Renamed, B, C, "/Game/B", "/Game/C", true}}});
		EXPECT_TRUE(State.Selected.contains("/Game/C/Asset.Asset"));
		EXPECT_TRUE(State.Selected.contains(C + "/file.txt"));
		EXPECT_TRUE(State.Selected.contains("/Game/AB/Keep.Keep"));
		EXPECT_TRUE(State.Selected.contains("/Game/a/Case.Case"));
		EXPECT_TRUE(State.Selected.contains("/Game/A.Package"));
		EXPECT_EQ(State.Anchor, "/Game/C/Asset.Asset");
		State.Apply({.Changes = {{EContentChangeKind::Removed, C, {}, "/Game/C/", {}, true}}});
		EXPECT_EQ(State.Selected.size(), 3u);
		EXPECT_TRUE(State.Anchor.empty());
	}
}

namespace Durin::Editor::ContentBrowser::Private
{
	TEST(FContentBrowserPanelExtensionTests, RealMenuQueriesCapabilitiesWithoutLoadingSavedPackage)
	{
		InitializeDObjectSystem();
		const auto Root = Testing::CreateTestFixtureDirectory("BrowserMenuNoLoad");
		const std::array Definitions{FMountPoint{.VirtualRoot = "/BrowserMenuNoLoad/", .Owner = EMountOwner::Test,
			.Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid());
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/BrowserMenuNoLoad/Asset", Path));
		DMaterial* Asset = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
		ASSERT_TRUE(SavePackage(Asset->GetPackage()));
		ASSERT_TRUE(UnloadPackage(Path));
		ASSERT_EQ(FindResidentPackage(Path), nullptr);
		const auto Data = FindAssetExact(Path);
		ASSERT_TRUE(Data);
		ImGuiContext* UI = ImGui::CreateContext();
		struct FCleanup { ImGuiContext* UI; ~FCleanup() { ImGui::DestroyContext(UI); } } Cleanup{UI};
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(800, 600);
		IO.DeltaTime = 1.0f / 60.0f;
		unsigned char* Pixels;
		int Width, Height;
		IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
		FContentBrowserPanel Panel({}, {}, {}, {}, {}, {}, {}, {}, {}, {});
		int Queries = 0;
		FContentBrowserPanelTestAccess::SetReimportQuery(Panel, [&](std::string_view ClassName) {
			++Queries;
			EXPECT_EQ(ClassName, Data->AssetClassName);
			const auto Actions = FReimportManager::QueryReimportActions(ClassName);
			return FReimportAvailability{Actions.bSupportsReimport, Actions.bSupportsReimportFromFile};
		});
		FContentBrowserPanelTestAccess::SetSelection(Panel, Root.generic_string(), {{
			.Kind = EContentBrowserItemKind::Asset, .Name = "Asset",
			.VirtualPath = Data->TopLevelAssets.front().AssetPath.ToString(), .PackagePath = Path,
			.PhysicalPath = Data->PhysicalPath, .AssetClassName = Data->AssetClassName}});
		for (int Frame = 0; Frame < 3; ++Frame)
		{
			ImGui::NewFrame();
			ImGui::Begin("Menu no-load test");
			FContentBrowserPanelTestAccess::DrawItemMenu(Panel);
			ImGui::End();
			ImGui::EndFrame();
			EXPECT_EQ(FindResidentPackage(Path), nullptr);
		}
		EXPECT_EQ(Queries, 3);
		Panel.StopRequestAdmission();
	}
}

namespace Durin::Editor::ContentBrowser::Private
{
	TEST(FContentBrowserPanelExtensionTests, DragPayloadSupportsMixedSelectionAndRetainsAssetCompatibility)
	{
		InitializeDObjectSystem();
		const std::string Root = Testing::CreateTestFixtureDirectory("BrowserDragPayload").generic_string();
		ImGuiContext* UI = ImGui::CreateContext();
		struct FCleanup { ImGuiContext* UI; ~FCleanup() { ImGui::DestroyContext(UI); } } Cleanup{UI};
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1200, 900);
		IO.DeltaTime = 1.0f / 60.0f;
		unsigned char* Pixels;
		int Width, Height;
		IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
		FContentBrowserPanel Panel({}, {}, {}, {}, {}, {}, {}, {},
			std::make_shared<FMountedContentReconciliationState>(), {});
		const FContentBrowserItem Asset{.Kind = EContentBrowserItemKind::Asset, .Name = "Asset",
			.VirtualPath = "/BrowserDrag/Asset.Asset", .PhysicalPath = Root + "/Asset.dasset"};
		const FContentBrowserItem File{.Kind = EContentBrowserItemKind::File, .Name = "notes.txt",
			.PhysicalPath = Root + "/notes.txt"};
		auto Drag = [&](std::vector<FContentBrowserItem> Items, const char* ExpectedType) {
			FContentBrowserPanelTestAccess::SetSelection(Panel, Root, std::move(Items));
			IO.MouseDown[0] = true;
			ImGui::NewFrame();
			ImGui::Begin("Drag test");
			ImGui::Button("Source");
			// Establish the same active-widget and threshold state as a pointer drag.
			ImGui::SetActiveID(UI->LastItemData.ID, ImGui::GetCurrentWindow());
			UI->ActiveIdMouseButton = 0;
			IO.MouseDragMaxDistanceSqr[0] = 10000;
			FContentBrowserPanelTestAccess::BeginDrag(Panel);
			const ImGuiPayload* Payload = ImGui::GetDragDropPayload();
			EXPECT_NE(Payload, nullptr);
			if (Payload)
			{
				EXPECT_TRUE(Payload->IsDataType(ExpectedType));
				if (Payload->IsDataType("DURIN_CONTENT_ITEMS"))
				{
					const std::string Paths(static_cast<const char*>(Payload->Data), Payload->DataSize);
					EXPECT_NE(Paths.find(File.PhysicalPath), std::string::npos);
					EXPECT_NE(Paths.find(Asset.PhysicalPath), std::string::npos);
				}
			}
			ImGui::End();
			ImGui::EndFrame();
			ImGui::ClearDragDrop();
			ImGui::ClearActiveID();
		};
		Drag({Asset, File}, "DURIN_CONTENT_ITEMS");
		Drag({Asset}, ::Durin::Editor::AssetDragDropPayloadType);
	}
}
