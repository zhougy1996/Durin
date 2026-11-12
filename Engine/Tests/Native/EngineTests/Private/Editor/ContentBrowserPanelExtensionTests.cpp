#include "Panels/ContentBrowserPanel.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorEngineTestSupport.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Class.h"
#include "NativePackageUnloadTestSupport.h"
#include "Editor/BrowserDefinitionTestSupport.h"
#include "EngineTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "Materials/Material.h"
#include "Import/EditorReimportHandler.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "imgui_internal.h"
#include "Asset/AssetDragDrop.h"

#include <gtest/gtest.h>

namespace Durin
{
	class FPanelProbeSource final : public DContentBrowserDataSource
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FPanelProbeSource, DContentBrowserDataSource, FPanelProbeSource::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FPanelProbeSource)
	public:
		explicit FPanelProbeSource(const FObjectInitializer& I = FObjectInitializer::Get())
			: Super(I)
		{
		}
		std::string Root = "/PanelProviders/";
		std::vector<std::string> Paths;
		int Saves = 0, Copies = 0, Moves = 0, Opens = 0, Renames = 0, Duplicates = 0, Deletes = 0;
		ContentBrowser::EContentBrowserItemSaveFlags LastFlags = ContentBrowser::EContentBrowserItemSaveFlags::None;
		std::function<void()> OnCanCopy;
		struct FPayload : ContentBrowser::IContentBrowserItemDataPayload
		{
			ContentBrowser::FContentBrowserHostContext Host;
		};
		struct FDelete : ContentBrowser::IContentBrowserDeleteRequest
		{
			auto CanExecute() const -> bool override { return true; }
		};
		auto GetDataSourceName() const -> FName override { return FName("PanelProbe"); }
		auto MakeItem(std::string Path, const ContentBrowser::FContentBrowserHostContext& Host) -> ContentBrowser::FContentBrowserItem
		{
			auto P = std::make_shared<FPayload>();
			P->Host = Host;
			ContentBrowser::FContentBrowserItem Item{.Kind = ContentBrowser::EContentBrowserItemKind::File, .Name = std::filesystem::path(Path).filename().string(), .VirtualPath = Path};
			Item.InternalItems.emplace_back(*this, Item.Kind, Path, std::move(P));
			return Item;
		}
		auto Allowed(const ContentBrowser::FContentBrowserItemData& Item) const -> bool
		{
			if (Item.GetOwnerDataSource() != this) return false;
			auto P = std::dynamic_pointer_cast<const FPayload>(Item.GetPayload());
			return P && (!P->Host.CanMutateContent || P->Host.CanMutateContent()) && Item.GetOwnerDataSource() == this;
		}
		auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter& Filter, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>& Callback, const ContentBrowser::FContentBrowserHostContext& Host) -> void override
		{
			const auto Current = Paths;
			for (const auto& Path : Current)
			{
				auto Parent = std::filesystem::path(Path).parent_path().generic_string() + "/";
				auto Query = Filter.Path;
				if (!Query.ends_with('/')) Query += '/';
				if ((Filter.Filter.bRecursive ? Path.starts_with(Query) : Parent == Query) && (!Filter.Filter.Kind || *Filter.Filter.Kind == ContentBrowser::EContentBrowserItemKind::File))
					if (!Callback(MakeItem(Path, Host))) return;
			}
		}
		auto CanSaveItem(const ContentBrowser::FContentBrowserItemData& Item, ContentBrowser::EContentBrowserItemSaveFlags, std::string*) const -> bool override { return Allowed(Item); }
		auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, ContentBrowser::EContentBrowserItemSaveFlags Flags) -> bool override
		{
			if (Items.empty() || !Allowed(Items.front())) return false;
			++Saves;
			LastFlags = Flags;
			return true;
		}
		auto CanEditItem(const ContentBrowser::FContentBrowserItemData& Item, std::string*) const -> bool override { return Item.GetOwnerDataSource() == this; }
		auto EditItem(const ContentBrowser::FContentBrowserItemData& Item) -> bool override
		{
			if (Item.GetOwnerDataSource() != this) return false;
			++Opens;
			return true;
		}
		auto CanRenameItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view Name, std::string*) const -> bool override { return Allowed(Item) && !Name.empty() && Name.find_first_of("/\\") == std::string_view::npos; }
		auto RenameItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view Name, ContentBrowser::FContentBrowserItem& Out) -> bool override
		{
			if (!CanRenameItem(Item, Name, nullptr)) return false;
			auto Old = Item.GetVirtualPath();
			auto New = std::filesystem::path(Old).parent_path().generic_string() + "/" + std::string(Name);
			auto It = std::ranges::find(Paths, Old);
			if (It == Paths.end()) return false;
			*It = New;
			auto P = std::dynamic_pointer_cast<const FPayload>(Item.GetPayload());
			Out = MakeItem(New, P->Host);
			++Renames;
			QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemMovedUpdate(Out.InternalItems.front(), Old));
			return true;
		}
		auto CanDuplicateItem(const ContentBrowser::FContentBrowserItemData& Item, std::string*) const -> bool override { return Allowed(Item); }
		auto DuplicateItem(const ContentBrowser::FContentBrowserItemData& Item, ContentBrowser::FContentBrowserItem& Out) -> bool override
		{
			if (!Allowed(Item)) return false;
			auto P = std::dynamic_pointer_cast<const FPayload>(Item.GetPayload());
			Paths.push_back(Item.GetVirtualPath() + "-copy");
			Out = MakeItem(Paths.back(), P->Host);
			++Duplicates;
			QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemAddedUpdate(Out.InternalItems.front()));
			return true;
		}
		auto CanCopyItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view, std::string*) const -> bool override
		{
			if (OnCanCopy) OnCanCopy();
			return Allowed(Item);
		}
		auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view Destination) -> bool override
		{
			if (Items.empty() || !std::ranges::all_of(Items, [&](const auto& Item) { return Allowed(Item); })) return false;
			++Copies;
			for (const auto& Item : Items)
			{
				auto P = std::dynamic_pointer_cast<const FPayload>(Item.GetPayload());
				auto Path = std::string(Destination);
				if (!Path.ends_with('/')) Path += '/';
				Path += std::filesystem::path(Item.GetVirtualPath()).filename().string() + "-copy";
				Paths.push_back(Path);
				QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemAddedUpdate(MakeItem(Path, P->Host).InternalItems.front()));
			}
			return true;
		}
		auto CanMoveItem(const ContentBrowser::FContentBrowserItemData& Item, std::string_view, std::string*) const -> bool override { return Allowed(Item); }
		auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, std::string_view) -> bool override
		{
			if (Items.empty() || !Allowed(Items.front())) return false;
			++Moves;
			return true;
		}
		auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData& Item, std::string*) const -> bool override { return Allowed(Item); }

		auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData> Items) -> ContentBrowser::FContentBrowserDeleteContext override
		{
			if (Items.empty() || !Allowed(Items.front())) return {};
			return {Items.front(), std::make_unique<FDelete>()};
		}
		auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData> Items, ContentBrowser::FContentBrowserDeleteContext& Context) -> bool override
		{
			if (Items.empty() || !Context.CanExecute() || !Allowed(Items.front())) return false;
			for (const auto& Item : Items)
			{
				std::erase(Paths, Item.GetVirtualPath());
				QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(Item));
			}
			++Deletes;
			Context.Request.reset();
			return true;
		}
	};
	auto FPanelProbeSource::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class) GetPrivateStaticClassBody("", "FPanelProbeSource", Class, nullptr, sizeof(FPanelProbeSource), alignof(FPanelProbeSource), EClassFlags::None, InternalConstructor<FPanelProbeSource>, &DContentBrowserDataSource::StaticClass);
		return Class;
	}
} // namespace Durin

namespace Durin::ContentBrowser::Private
{
	// Exercises production UI entrypoints without creating an OS window or starting a renderer.
	struct FContentBrowserPanelTestAccess
	{
		static auto ApplyPreferences(FContentBrowserPanel& Panel, const FPresentationSettings& Settings) -> bool
		{ return Panel.ApplyPreferences(Settings); }
		static auto Preferences(const FContentBrowserPanel& Panel) -> FPresentationSettings
		{
			FPresentationSettings Result = Panel.PresentationSettings;
			Result.ViewMode = static_cast<uint8>(Panel.ViewMode);
			Result.IconSize = Panel.IconSize;
			Result.bIconSizeLocked = Panel.bIconSizeLocked;
			Result.bShowHiddenFiles = Panel.Model.IsShowingHiddenFiles();
			return Result;
		}
		static auto Copy(FContentBrowserPanel& Panel) -> void { Panel.CopyContentSelection(); }
		static auto Paste(FContentBrowserPanel& Panel, std::string_view Folder) -> void { Panel.PasteContent(Folder); }
		static auto CanPaste(const FContentBrowserPanel& Panel) -> bool { return Panel.HasContentClipboard(); }
		static auto CreateFolder(FContentBrowserPanel& Panel, const std::string& Root) -> void
		{
			Panel.CreateFolder(Root);
		}
		static auto CommitFolder(FContentBrowserPanel& Panel, std::string_view Name) -> bool
		{
			if (!Panel.PendingFolder) return false;
			const auto Item = Panel.PendingFolder->Context.GetItem();
			Panel.RenameBuffer.fill(0);
			std::memcpy(Panel.RenameBuffer.data(), Name.data(), std::min(Name.size(), Panel.RenameBuffer.size() - 1));
			return Panel.CommitRename(Item);
		}
		static auto CancelRename(FContentBrowserPanel& Panel) -> void { Panel.CancelRename(); }
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
		static auto HasVisibleItem(const FContentBrowserPanel& Panel, const std::string& Path) -> bool
		{
			return std::ranges::any_of(Panel.Model.GetItems(), [&](const auto& Item) { return Item.PhysicalPath == Path; });
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
		static auto SetView(FContentBrowserPanel& Panel, EContentBrowserViewMode Mode) -> void { Panel.ViewMode = Mode; }
		static auto HasDragSnapshot(const FContentBrowserPanel& Panel, size_t Count) -> bool { return Panel.DragSnapshot && Panel.DragSnapshot->Items.size() == Count; }

		static auto Directory(const FContentBrowserPanel& Panel) -> std::string { return Panel.Model.GetCurrentVirtualPath() + " | " + Panel.Model.GetCurrentPhysicalPath(); }
		static auto Items(const FContentBrowserPanel& Panel) -> std::vector<FContentBrowserItem> { return {Panel.Model.GetItems().begin(), Panel.Model.GetItems().end()}; }
		static auto Select(FContentBrowserPanel& Panel, std::string_view Path) -> FContentBrowserItem
		{
			Panel.SelectionState.Selected.clear();
			for (const auto& Item : Panel.Model.GetItems())
				if (Item.VirtualPath == Path)
				{
					Panel.SelectionState.Selected.insert(Item.StableId());
					return Item;
				}
			return {};
		}
		static auto Rename(FContentBrowserPanel& Panel, const FContentBrowserItem& Item, std::string_view Name) -> bool
		{
			Panel.BeginRename(Item);
			Panel.RenameBuffer.fill(0);
			std::memcpy(Panel.RenameBuffer.data(), Name.data(), std::min(Name.size(), Panel.RenameBuffer.size() - 1));
			return Panel.CommitRename(Item);
		}
		static auto Requests(const FContentBrowserPanel& Panel) -> uint64 { return Panel.Model.GetItemsRequestCount(); }
		static auto Suspend(FContentBrowserPanel& Panel, bool Suspended) -> void { Panel.Model.SetSnapshotPublicationSuspended(Suspended); }
		static auto Pump(FContentBrowserPanel& Panel) -> void { Panel.Model.PumpPendingSnapshots(); }

		static auto Save(FContentBrowserPanel& Panel, const FContentBrowserItem& Item) -> void { Panel.SaveItem(Item); }
		static auto Open(FContentBrowserPanel& Panel, const FContentBrowserItem& Item) -> void { Panel.OpenItem(Item); }
		static auto Duplicate(FContentBrowserPanel& Panel, const FContentBrowserItem& Item) -> void { Panel.DuplicateItem(Item); }
		static auto Move(FContentBrowserPanel& Panel, std::vector<FContentBrowserItem> Items, std::string Destination) -> bool { return Panel.MoveItems(std::move(Items), std::move(Destination)); }
		static auto PrepareDelete(FContentBrowserPanel& Panel) -> void { Panel.RequestDeleteSelection(); }
		static auto ExecuteDelete(FContentBrowserPanel& Panel) -> void { Panel.DeleteSelection(); }
		static auto DeletionPreview(const FContentBrowserPanel& Panel) -> std::shared_ptr<const FContentBrowserDeletePreview> { return Panel.GetDeletionPreview(); }

		static auto Delete(FContentBrowserPanel& Panel) -> void
		{
			Panel.RequestDeleteSelection();
			Panel.DeleteSelection();
		}
		static auto SetMutationAllowed(FContentBrowserPanel& Panel, bool Allowed) -> void { Panel.bAllowAssetMutation = Allowed; }

		static auto BeginDrag(FContentBrowserPanel& Panel) -> void
		{ Panel.BeginAssetDragDrop(Panel.Model.GetItems().front()); }
		static auto DrawItemMenu(FContentBrowserPanel& Panel) -> void
		{
			Panel.DrawItemContextMenu(Panel.Model.GetItems().front());
		}
	};

	class FContentBrowserPanelExtensionTests : public testing::Test
	{
	protected:
		TStrongObjectPtr<DEditorEngine> Editor;
		DContentBrowserDataSubsystem* BrowserData = nullptr;
		std::vector<TStrongObjectPtr<FPanelProbeSource>> Sources;
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			FModuleManager::Get().LoadModuleChecked("ContentBrowser");
			Editor = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
			FEditorEngineTestAccess::StartEditor(*Editor);
			BrowserData = Editor->GetEditorSubsystem<DContentBrowserDataSubsystem>();
			ASSERT_NE(nullptr, BrowserData);
		}
		auto AddProbe(std::string Root) -> FPanelProbeSource*
		{
			auto* Source = NewObject<FPanelProbeSource>(BrowserData, {});
			Source->Root = std::move(Root);
			Source->Paths = {Source->Root + "item.note"};
			Sources.emplace_back(Source);
			Source->Initialize();
			if (!BrowserData->ActivateDataSource(Source->GetDataSourceName())) return nullptr;
			return Source;
		}

		auto TearDown() -> void override
		{
			for (auto& Source : Sources)
				Source->Shutdown();
			if (Editor.Get()) Editor->PreExit();
			Sources.clear();
			BrowserData = nullptr;
			Editor.Reset();
		}
	};

	TEST_F(FContentBrowserPanelExtensionTests, PreferenceSaveFailureLeavesActivePresentationUnchanged)
	{
		InitializeDObjectSystem();
		bool bAllowSave = false;
		FPresentationSettings Saved;
		FContentBrowserPanel Panel({}, [&](const FPresentationSettings& Settings) {
			if (!bAllowSave) return false;
			Saved = Settings;
			return true;
		},
								   {.DataSubsystem = BrowserData});
		const auto Previous = FContentBrowserPanelTestAccess::Preferences(Panel);
		auto Next = Previous;
		Next.ViewMode = 1;
		Next.IconSize = 120.0f;
		Next.bIconSizeLocked = true;
		Next.bShowHiddenFiles = true;
		EXPECT_FALSE(FContentBrowserPanelTestAccess::ApplyPreferences(Panel, Next));
		EXPECT_EQ(FContentBrowserPanelTestAccess::Preferences(Panel), Previous);
		bAllowSave = true;
		EXPECT_TRUE(FContentBrowserPanelTestAccess::ApplyPreferences(Panel, Next));
		EXPECT_EQ(FContentBrowserPanelTestAccess::Preferences(Panel), Next);
		EXPECT_EQ(Saved, Next);
	}

	TEST_F(FContentBrowserPanelExtensionTests, CopiesOrdinarySelectionAcrossPanelsAndInvalidatesReplacedClipboard)
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
		Tests::FTestTransactorOwner Transactions;
		int UnscopedNotifications = 0;
		FContentBrowserHostContext Host{
			.GetMountedContentMutationRevision = [&] { return Transactions->GetMountedContentMutationRevision(); },
			.NotifyMountedContentMutation = [&] { ++UnscopedNotifications; },
			.CaptureMountedContentChanges = [&](uint64 From) { return Transactions->CaptureMountedContentChanges(From); },
			.NotifyScopedContentMutation = [&](FContentChangeBatch Changes) { Transactions->NotifyMountedContentMutation(std::move(Changes)); },
			.ReconciliationState = std::make_shared<FMountedContentReconciliationState>()};
		Host.DataSubsystem = BrowserData;
		FContentBrowserPanel Source({}, {}, Host);
		FContentBrowserPanel Target({}, {}, Host);
		const auto InitialRevision = Transactions->GetMountedContentMutationRevision();
		FContentBrowserPanelTestAccess::SetSelection(Source, Root.generic_string(), {
			{.Kind = EContentBrowserItemKind::File, .Name = "notes.txt", .PhysicalPath = (Root / "notes.txt").generic_string()},
			{.Kind = EContentBrowserItemKind::File, .Name = "config.json", .PhysicalPath = (Root / "config.json").generic_string()}});
		FContentBrowserPanelTestAccess::Copy(Source);
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Source, (Root / "Destination").generic_string()));
		Source.TickWhenHidden();
		FContentBrowserPanelTestAccess::FinishCapture(Source);
		EXPECT_FALSE(FContentBrowserPanelTestAccess::HasVisibleItem(Source, (Root / "Destination/notes.txt").generic_string()));
		ASSERT_TRUE(FContentBrowserPanelTestAccess::CanPaste(Target));
		FContentBrowserPanelTestAccess::Paste(Target, "/BrowserContentClipboard/Destination/");
		EXPECT_TRUE(std::filesystem::exists(Root / "Destination/notes.txt"));
		EXPECT_TRUE(std::filesystem::exists(Root / "Destination/config.json"));
		EXPECT_EQ(Transactions->GetMountedContentMutationRevision(), InitialRevision + 1);
		Source.TickWhenHidden();
		FContentBrowserPanelTestAccess::FinishCapture(Source);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::HasVisibleItem(Source, (Root / "Destination/notes.txt").generic_string()));
		EXPECT_EQ(UnscopedNotifications, 0);
		FContentBrowserPanelTestAccess::Paste(Target, "/BrowserContentClipboard/Destination/");
		EXPECT_TRUE(std::filesystem::exists(Root / "Destination/notes_Copy.txt"));
		EXPECT_EQ(Transactions->GetMountedContentMutationRevision(), InitialRevision + 2);
		EXPECT_EQ(UnscopedNotifications, 0);
		ImGui::SetClipboardText("unrelated text");
		EXPECT_FALSE(FContentBrowserPanelTestAccess::CanPaste(Target));
	}

	TEST_F(FContentBrowserPanelExtensionTests, TemporaryFolderCreatesOnlyAfterConfirmationAndCancelsWithoutWriting)
	{
		InitializeDObjectSystem();
		const auto Root = Testing::CreateTestFixtureDirectory("BrowserDeferredRename");
		std::filesystem::create_directory(Root / "Other");
		const std::array Definitions{FMountPoint{
			.VirtualRoot = "/BrowserDeferredRename/", .Owner = EMountOwner::Test,
			.Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid());
		ImGuiContext* UI = ImGui::CreateContext();
		struct FContextCleanup { ImGuiContext* Context; ~FContextCleanup() { ImGui::DestroyContext(Context); } } Cleanup{UI};
		auto& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1200, 900);
		IO.DeltaTime = 1.0f / 60.0f;
		unsigned char* Pixels;
		int Width, Height;
		IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
		FContentBrowserPanel Panel({}, {}, {.ReconciliationState = std::make_shared<FMountedContentReconciliationState>(), .DataSubsystem = BrowserData});
		FContentBrowserPanelTestAccess::CreateFolder(Panel, Root.generic_string());
		EXPECT_FALSE(FContentBrowserPanelTestAccess::RenameTarget(Panel).empty());
		EXPECT_FALSE(std::filesystem::exists(Root / "New Folder"));
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		EXPECT_FALSE(std::filesystem::exists(Root / "New Folder"));
		EXPECT_FALSE(FContentBrowserPanelTestAccess::CommitFolder(Panel, "../Escape"));
		EXPECT_FALSE(FContentBrowserPanelTestAccess::RenameTarget(Panel).empty());
		// A collision after preparation must not be treated as successful creation.
		std::ofstream(Root / "Occupied") << "existing";
		EXPECT_FALSE(FContentBrowserPanelTestAccess::CommitFolder(Panel, "Occupied"));
		ASSERT_TRUE(FContentBrowserPanelTestAccess::CommitFolder(Panel, "Chosen"));
		EXPECT_TRUE(std::filesystem::is_directory(Root / "Chosen"));
		EXPECT_FALSE(std::filesystem::exists(Root / "New Folder"));
		EXPECT_TRUE(FContentBrowserPanelTestAccess::IsSelected(Panel, (Root / "Chosen").generic_string()));
		FContentBrowserPanelTestAccess::CreateFolder(Panel, Root.generic_string());
		FContentBrowserPanelTestAccess::CancelRename(Panel);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::RenameTarget(Panel).empty());
		EXPECT_FALSE(std::filesystem::exists(Root / "New Folder"));
		FContentBrowserPanelTestAccess::CreateFolder(Panel, Root.generic_string());
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Panel, (Root / "Other").generic_string()));
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::RenameTarget(Panel).empty());
		EXPECT_FALSE(std::filesystem::exists(Root / "New Folder"));
		FContentBrowserPanelTestAccess::CreateFolder(Panel, Root.generic_string());
		ASSERT_TRUE(FContentBrowserPanelTestAccess::CommitFolder(Panel, "New Folder"));
		EXPECT_TRUE(std::filesystem::is_directory(Root / "New Folder"));
		for (const auto Mode : {EContentBrowserViewMode::Grid, EContentBrowserViewMode::Details})
		{
			FContentBrowserPanelTestAccess::SetView(Panel, Mode);
			FContentBrowserPanelTestAccess::CreateFolder(Panel, Root.generic_string());
			auto Draw = [&] {
				ImGui::NewFrame();
				ImGui::SetNextWindowSize(ImVec2(1100, 800));
				ImGui::Begin("Temporary folder test");
				FContentBrowserPanelTestAccess::DrawContent(Panel);
				ImGui::End();
				ImGui::EndFrame();
			};
			Draw();
			Draw();
			EXPECT_FALSE(FContentBrowserPanelTestAccess::RenameTarget(Panel).empty());
			IO.AddKeyEvent(ImGuiKey_Escape, true);
			Draw();
			EXPECT_TRUE(FContentBrowserPanelTestAccess::RenameTarget(Panel).empty());
			EXPECT_FALSE(std::filesystem::exists(Root / "New Folder (2)"));
			IO.AddKeyEvent(ImGuiKey_Escape, false);
			Draw();
		}
		// The target can disappear while the name editor is open.
		std::filesystem::create_directory(Root / "Transient");
		FContentBrowserPanelTestAccess::CreateFolder(Panel, (Root / "Transient").generic_string());
		std::filesystem::remove(Root / "Transient");
		EXPECT_FALSE(FContentBrowserPanelTestAccess::CommitFolder(Panel, "Child"));
		EXPECT_FALSE(std::filesystem::exists(Root / "Transient"));
		FContentBrowserPanelTestAccess::CancelRename(Panel);

	}

	TEST_F(FContentBrowserPanelExtensionTests, DrawsRegisteredDetailsAndContextMenuInRealPanel)
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

		FContentBrowserPanel Panel({}, {}, {.ReconciliationState = std::make_shared<FMountedContentReconciliationState>(), .DataSubsystem = BrowserData});
		std::string Error;
		int TypeDetailCalls = 0;
		auto* Definition = NewObject<FBrowserTestDefinition>(nullptr, {});
		Definition->ClassPath = "PanelTest::DNovelAsset"; Definition->DisplayName = "Novel asset"; Definition->Icon = "*";
		auto DefinitionRegistration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(DefinitionRegistration);
		auto Type = RegisterAssetTypeExtras({
			.AssetClassName = Definition->ClassPath,
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
		DefinitionRegistration.Reset();
		EXPECT_EQ(Draw(true).find("Custom context rendered"), std::string::npos);
		EXPECT_EQ(Draw(false).find("Multiple details rendered"), std::string::npos);
		Panel.StopRequestAdmission();
	}
}

namespace Durin::ContentBrowser::Private
{
	TEST_F(FContentBrowserPanelExtensionTests, SelectionMigratesOrderedRenamesAndRemovesDeletedSubtrees)
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

namespace Durin::ContentBrowser::Private
{
	TEST_F(FContentBrowserPanelExtensionTests, RealMenuQueriesCapabilitiesWithoutLoadingSavedPackage)
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
		const auto Data = FindPackageExact(Path);
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
		int Queries = 0;
		FContentBrowserPanel Panel({}, {}, {.QueryReimport = [&](std::string_view ClassName) {
												++Queries;
												EXPECT_EQ(ClassName, Data->TopLevelAssets.front().AssetClassName);
												const auto Actions = FReimportManager::QueryReimportActions(ClassName);
												return FReimportAvailability{Actions.bSupportsReimport, Actions.bSupportsReimportFromFile};
											},
											.DataSubsystem = BrowserData});
		FContentBrowserPanelTestAccess::SetSelection(Panel, Root.generic_string(), {{
			.Kind = EContentBrowserItemKind::Asset, .Name = "Asset",
			.VirtualPath = Data->TopLevelAssets.front().AssetPath.ToString(), .PackagePath = Path,
			.PhysicalPath = Data->PhysicalPath, .AssetClassName = Data->TopLevelAssets.front().AssetClassName}});
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

namespace Durin::ContentBrowser::Private
{
	TEST_F(FContentBrowserPanelExtensionTests, DragPayloadSupportsMixedSelectionAndRetainsAssetCompatibility)
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
		FContentBrowserPanel Panel({}, {}, {.ReconciliationState = std::make_shared<FMountedContentReconciliationState>(), .DataSubsystem = BrowserData});
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
					EXPECT_EQ(Payload->DataSize, sizeof(uint64));
					EXPECT_TRUE(FContentBrowserPanelTestAccess::HasDragSnapshot(Panel, 2));
				}
			}
			ImGui::End();
			ImGui::EndFrame();
			ImGui::ClearDragDrop();
			ImGui::ClearActiveID();
		};
		Drag({Asset, File}, "DURIN_CONTENT_ITEMS");
		Drag({Asset}, ::Durin::AssetDragDropPayloadType);
	}
	TEST_F(FContentBrowserPanelExtensionTests, VirtualProviderUsesOrdinaryCommandsAndRefreshesIndependentBrowsers)
	{
		const auto Root = Testing::CreateTestFixtureDirectory("PanelProviderCommands");
		const std::array Mounts{FMountPoint{.VirtualRoot = "/PanelProviders/", .Owner = EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Mounts);
		ASSERT_TRUE(Registry.IsValid());
		auto* Probe = AddProbe("/PanelProviders/");
		ASSERT_NE(nullptr, Probe);
		FContentBrowserHostContext Host{.DataSubsystem = BrowserData};
		FContentBrowserPanel First({}, {}, Host), Second({}, {}, Host);
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(First, Root.generic_string()));
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Second, Root.generic_string()));
		FContentBrowserPanelTestAccess::FinishCapture(First);
		FContentBrowserPanelTestAccess::FinishCapture(Second);
		auto Item = FContentBrowserPanelTestAccess::Select(First, "/PanelProviders/item.note");
		ASSERT_EQ(1u, Item.InternalItems.size());
		EXPECT_TRUE(Item.PhysicalPath.empty());
		EXPECT_FALSE(FContentBrowserPanelTestAccess::IsSelected(Second, Item.StableId()));
		FContentBrowserPanelTestAccess::Select(Second, Item.VirtualPath);
		FContentBrowserPanelTestAccess::Save(First, Item);
		FContentBrowserPanelTestAccess::Open(First, Item);
		EXPECT_EQ(1, Probe->Saves);
		EXPECT_EQ(1, Probe->Opens);
		EXPECT_EQ(EContentBrowserItemSaveFlags::SaveOnlyIfDirty | EContentBrowserItemSaveFlags::SaveOnlyIfLoaded, Probe->LastFlags);
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Rename(First, Item, "renamed.note"));
		FContentBrowserPanelTestAccess::FinishCapture(First);
		Second.TickWhenHidden();
		FContentBrowserPanelTestAccess::FinishCapture(Second);
		const auto Rows = FContentBrowserPanelTestAccess::Items(Second);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::IsSelected(Second, "/PanelProviders/renamed.note"));
		EXPECT_TRUE(std::ranges::any_of(Rows, [](const auto& Row) { return Row.VirtualPath == "/PanelProviders/renamed.note"; }));
		EXPECT_FALSE(std::ranges::any_of(Rows, [](const auto& Row) { return Row.VirtualPath == "/PanelProviders/item.note"; }));
		auto Renamed = FContentBrowserPanelTestAccess::Select(First, "/PanelProviders/renamed.note");
		ASSERT_FALSE(Renamed.InternalItems.empty()) << "First view lost the renamed provider item: " << FContentBrowserPanelTestAccess::Directory(First);
		ASSERT_EQ(Probe, Renamed.InternalItems.front().GetOwnerDataSource());
		ASSERT_TRUE(Renamed.CanDuplicate());
		FContentBrowserPanelTestAccess::Duplicate(First, Renamed);
		EXPECT_EQ(1, Probe->Duplicates);
		First.TickWhenHidden();
		FContentBrowserPanelTestAccess::FinishCapture(First);
		FContentBrowserPanelTestAccess::Select(First, "/PanelProviders/renamed.note-copy");
		FContentBrowserPanelTestAccess::Delete(First);
		EXPECT_EQ(1, Probe->Deletes);
		EXPECT_TRUE(std::filesystem::is_empty(Root));
	}
	TEST_F(FContentBrowserPanelExtensionTests, ClipboardAndDropUseTargetPolicyAndRejectRetiredIdentities)
	{
		const auto Root = Testing::CreateTestFixtureDirectory("PanelProviderTransfer");
		std::filesystem::create_directory(Root / "Target");
		const std::array Mounts{FMountPoint{.VirtualRoot = "/PanelProviders/", .Owner = EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Mounts);
		ASSERT_TRUE(Registry.IsValid());
		auto* Probe = AddProbe("/PanelProviders/");
		ASSERT_NE(nullptr, Probe);
		auto* UI = ImGui::CreateContext();
		struct FCleanup
		{
			ImGuiContext* UI;
			~FCleanup() { ImGui::DestroyContext(UI); }
		} Cleanup{UI};
		std::string Clipboard;
		auto& Platform = ImGui::GetPlatformIO();
		Platform.Platform_ClipboardUserData = &Clipboard;
		Platform.Platform_GetClipboardTextFn = [](ImGuiContext* C) -> const char* { return static_cast<std::string*>(C->PlatformIO.Platform_ClipboardUserData)->c_str(); };
		Platform.Platform_SetClipboardTextFn = [](ImGuiContext* C, const char* Text) { *static_cast<std::string*>(C->PlatformIO.Platform_ClipboardUserData) = Text; };
		FContentBrowserHostContext Host{.DataSubsystem = BrowserData};
		FContentBrowserPanel Source({}, {}, Host), Target({}, {}, Host);
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Source, Root.generic_string()));
		FContentBrowserPanelTestAccess::FinishCapture(Source);
		auto Item = FContentBrowserPanelTestAccess::Select(Source, "/PanelProviders/item.note");
		ASSERT_FALSE(Item.InternalItems.empty());
		FContentBrowserPanelTestAccess::Copy(Source);
		FContentBrowserPanelTestAccess::SetMutationAllowed(Target, false);
		FContentBrowserPanelTestAccess::Paste(Target, "/PanelProviders/Target/");
		EXPECT_EQ(0, Probe->Copies);
		FContentBrowserPanelTestAccess::SetMutationAllowed(Target, true);
		Probe->OnCanCopy = [&] { FContentBrowserPanelTestAccess::SetMutationAllowed(Target, false); };
		FContentBrowserPanelTestAccess::Paste(Target, "/PanelProviders/Target/");
		EXPECT_EQ(0, Probe->Copies);
		Probe->OnCanCopy = {};
		FContentBrowserPanelTestAccess::SetMutationAllowed(Target, true);
		FContentBrowserPanelTestAccess::Paste(Target, "/PanelProviders/Target/");
		EXPECT_EQ(1, Probe->Copies);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::Move(Target, {Item}, "/PanelProviders/Target/"));
		EXPECT_EQ(1, Probe->Moves);
		ASSERT_TRUE(BrowserData->DeactivateDataSource(FName("PanelProbe")));
		ASSERT_TRUE(BrowserData->ActivateDataSource(FName("PanelProbe")));
		FContentBrowserPanelTestAccess::Paste(Target, "/PanelProviders/Target/");
		EXPECT_EQ(1, Probe->Copies);
		EXPECT_FALSE(FContentBrowserPanelTestAccess::Move(Target, {Item}, "/PanelProviders/Target/"));
		EXPECT_EQ(1, Probe->Moves);
		Target.StopRequestAdmission();
		FContentBrowserPanelTestAccess::Open(Target, Item);
		EXPECT_EQ(0, Probe->Opens);
	}
	TEST_F(FContentBrowserPanelExtensionTests, ProviderWithdrawalClearsRowsAndPreventsCommandsFromReviving)
	{
		const auto Root = Testing::CreateTestFixtureDirectory("PanelProviderRetirement");
		const std::array Mounts{FMountPoint{.VirtualRoot = "/PanelProviders/", .Owner = EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Mounts);
		ASSERT_TRUE(Registry.IsValid());
		auto* Probe = AddProbe("/PanelProviders/");
		ASSERT_NE(nullptr, Probe);
		FContentBrowserPanel Panel({}, {}, {.DataSubsystem = BrowserData});
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Panel, Root.generic_string()));
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		auto Item = FContentBrowserPanelTestAccess::Select(Panel, "/PanelProviders/item.note");
		ASSERT_FALSE(Item.InternalItems.empty());
		Probe->Shutdown();
		Panel.TickWhenHidden();
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::Items(Panel).empty());
		EXPECT_FALSE(FContentBrowserPanelTestAccess::Rename(Panel, Item, "late.note"));
		FContentBrowserPanelTestAccess::Save(Panel, Item);
		EXPECT_EQ(0, Probe->Saves);
		EXPECT_EQ(0, Probe->Renames);
		Editor->PreExit();
		Panel.TickWhenHidden();
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		EXPECT_TRUE(FContentBrowserPanelTestAccess::Items(Panel).empty());
	}
	TEST_F(FContentBrowserPanelExtensionTests, NativeDeletionRefreshesConfirmationBeforeRetry)
	{
		const auto Root = Testing::CreateTestFixtureDirectory("PanelConfirmedDeletion");
		std::ofstream(Root / "notes.txt") << "original";
		const std::array Mounts{FMountPoint{.VirtualRoot = "/PanelDeletion/", .Owner = EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Mounts);
		ASSERT_TRUE(Registry.IsValid());
		FContentBrowserPanel Panel({}, {}, {.DataSubsystem = BrowserData});
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Panel, Root.generic_string()));
		FContentBrowserPanelTestAccess::FinishCapture(Panel);
		auto Item = FContentBrowserPanelTestAccess::Select(Panel, "/PanelDeletion/notes.txt");
		ASSERT_FALSE(Item.InternalItems.empty());
		FContentBrowserPanelTestAccess::PrepareDelete(Panel);
		const auto Preview = FContentBrowserPanelTestAccess::DeletionPreview(Panel);
		ASSERT_NE(nullptr, Preview);
		EXPECT_EQ(1u, Preview->Summary.FileCount);
		std::ofstream(Root / "notes.txt", std::ios::app) << "changed";
		FContentBrowserPanelTestAccess::ExecuteDelete(Panel);
		EXPECT_TRUE(std::filesystem::exists(Root / "notes.txt"));
		EXPECT_NE(Preview, FContentBrowserPanelTestAccess::DeletionPreview(Panel));
		FContentBrowserPanelTestAccess::ExecuteDelete(Panel);
		EXPECT_FALSE(std::filesystem::exists(Root / "notes.txt"));
		EXPECT_EQ(nullptr, FContentBrowserPanelTestAccess::DeletionPreview(Panel));
	}

	TEST_F(FContentBrowserPanelExtensionTests, UnrelatedUpdatesPreserveOtherViewAndRespectPublicationFence)
	{
		const auto Root = Testing::CreateTestFixtureDirectory("PanelProviderUpdateScope");
		std::filesystem::create_directory(Root / "Else");
		const std::array Mounts{FMountPoint{.VirtualRoot = "/PanelProviders/", .Owner = EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Mounts);
		ASSERT_TRUE(Registry.IsValid());
		auto* Probe = AddProbe("/PanelProviders/");
		ASSERT_NE(nullptr, Probe);
		FContentBrowserHostContext Host{.DataSubsystem = BrowserData};
		FContentBrowserPanel First({}, {}, Host), Second({}, {}, Host);
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(First, Root.generic_string()));
		ASSERT_TRUE(FContentBrowserPanelTestAccess::Navigate(Second, (Root / "Else").generic_string()));
		FContentBrowserPanelTestAccess::FinishCapture(First);
		FContentBrowserPanelTestAccess::FinishCapture(Second);
		const auto Before = FContentBrowserPanelTestAccess::Requests(First), OtherBefore = FContentBrowserPanelTestAccess::Requests(Second);
		Probe->Paths.push_back("/PanelProviders/Else/new.note");
		auto Added = Probe->MakeItem(Probe->Paths.back(), Host);
		Probe->QueueItemDataUpdate(FContentBrowserItemDataUpdate::MakeItemAddedUpdate(Added.InternalItems.front()));
		FContentBrowserPanelTestAccess::Pump(First);
		EXPECT_EQ(Before, FContentBrowserPanelTestAccess::Requests(First));
		FContentBrowserPanelTestAccess::Suspend(Second, true);
		FContentBrowserPanelTestAccess::Pump(Second);
		EXPECT_EQ(OtherBefore, FContentBrowserPanelTestAccess::Requests(Second));
		FContentBrowserPanelTestAccess::Suspend(Second, false);
		FContentBrowserPanelTestAccess::FinishCapture(Second);
		EXPECT_GT(FContentBrowserPanelTestAccess::Requests(Second), OtherBefore);
		EXPECT_FALSE(FContentBrowserPanelTestAccess::Select(Second, "/PanelProviders/Else/new.note").InternalItems.empty());
	}
}
