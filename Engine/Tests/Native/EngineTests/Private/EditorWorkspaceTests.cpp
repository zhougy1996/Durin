#include "ThumbnailManagerTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Asset/Asset.h"
#include "Feedback/SlowTaskWidget.h"
#include "Misc/SlowTask.h"
#include "Asset/PackageRemoval.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Actors/ActorChange.h"
#include "AssetRegistry/Publication.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Asset/AssetPicker.h"
#include "Editor/EditorEngine.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Transactions/Transaction.h"
#include "Transactions/Transactor.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Toolkits/AssetEditorSession.h"
#include "Framework/TabLayout.h"
#include "Framework/TabManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "StaticMesh/StaticMesh.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "MonaImGui.h"
#include "Widgets/MWindow.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	// Admission-only renderer: the picker must leave generation queued after Draw.
	class FPickerThumbnailRenderer final : public Durin::DThumbnailRenderer
	{
		DURIN_TEST_THUMBNAIL_RENDERER(FPickerThumbnailRenderer)
	public:
		auto CanVisualizeAsset(Durin::DObject*) const -> bool override { return true; }
		auto Draw(Durin::DObject*, Durin::FThumbnailDrawContext&) -> void override { ADD_FAILURE() << "Unexpected drawing."; }

	};

	class FAppliedPackageEdit final : public Durin::FCommandChange
	{
	public:
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override
		{ return {}; }

		explicit FAppliedPackageEdit(Durin::DPackage& InPackage)
			: Package{&InPackage}
		{
		}

		auto GetDescription() const -> std::string_view override { return "Edit Asset"; }
		auto GetAffectedPackages() const -> std::span<Durin::DPackage* const> override
		{
			return Package;
		}
		auto Revert() -> void override
		{}
		auto Apply() -> void override
		{}

	private:
		std::array<Durin::DPackage*, 1> Package{};
	};


}

TEST(FEditorTransactorOwnershipTests, OwnsTransientBufferAndClearsOpenHistoryOnShutdown)
{
	std::expected<void, Durin::FPropertySnapshotError> SnapshotResult;
	Durin::Testing::InitializeAssetSystemForTests();
	auto* Editor = Durin::NewObject<Durin::DEditorEngine>(nullptr, "TransactorOwnerEditor");
	ASSERT_NE(Editor, nullptr);
	auto* Buffer = Durin::Cast<Durin::DTransBuffer>(Editor->GetTransactor());
	ASSERT_NE(Buffer, nullptr);
	EXPECT_EQ(Buffer->GetOuter(), Editor);
	EXPECT_TRUE(Buffer->HasAnyObjectFlags(Durin::EObjectFlags::Transient));
	Durin::FProperty* TransProperty = Editor->GetClass()->FindPropertyByName("Trans");
	ASSERT_NE(TransProperty, nullptr);
	EXPECT_TRUE(TransProperty->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient));
	{
		Durin::FScopedTransaction Missing("No active editor");
		EXPECT_FALSE(Missing.IsOutstanding());
		EXPECT_FALSE(Buffer->IsActive());
	}
	ASSERT_TRUE(Editor->ActivateEditor());
	{
		Durin::FScopedTransaction Scope("Editor transaction");
		ASSERT_TRUE(Scope.IsOutstanding());
		Scope.Cancel();
		EXPECT_FALSE(Scope.IsOutstanding());
	}

	const auto Scope = Buffer->Begin("test", "Open during shutdown");
	ASSERT_GE(Scope, 0);
	Buffer->SetPrimaryUndoObject(Editor);
	const Durin::FPropertyEditTarget Target =
		Durin::FPropertyEditTarget::ForMember(Editor, TransProperty);
	Durin::FPropertyValueSnapshotPayload Snapshot;
	std::string Error;
	ASSERT_TRUE((SnapshotResult = Durin::CapturePropertyValuePayload(TransProperty, Editor, 0, Snapshot))) << Durin::ToString(SnapshotResult.error());
	Editor->SetFlags(Durin::EObjectFlags::Transactional);
	ASSERT_TRUE(Editor->Modify(false));
	Editor->BeginDestroy();
	EXPECT_EQ(Editor->GetTransactor(), nullptr);
	EXPECT_FALSE(Buffer->CanTransact());
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Buffer->GetUndoSize(), 0u);
}

TEST(FAssetEditorSessionTests, UndoToActivatedRevisionClearsDirtyState)
{
	Durin::Testing::InitializeAssetSystemForTests();
	auto* Editor = Durin::NewObject<Durin::DEditorEngine>(
		nullptr, "EditableAssetDocumentEditor");
	ASSERT_NE(Editor, nullptr);
	ASSERT_TRUE(Editor->ActivateEditor());
	Durin::Testing::FScopedMountRegistryFixture MountFixture;
	Durin::Testing::RegisterMountPointForTests(
		"/EditableAssetDocumentTests/",
		Durin::Testing::GetTestWorkDirectory().generic_string() + "/");
	Durin::FPackagePath PackagePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/EditableAssetDocumentTests/UndoCheckpoint", PackagePath));
	Durin::DPackage* Package = Durin::NewObject<Durin::DPackage>(
		nullptr, "UndoCheckpointPackage");
	ASSERT_NE(Package, nullptr);
	Package->InitializeAssetPackage(PackagePath);
	Durin::DObject* Asset = Durin::NewObject<Durin::DObject>(Package, "Asset");
	ASSERT_NE(Asset, nullptr);
	Package->ClearDirty();

	Durin::FAssetEditorSession Documents;
	ASSERT_TRUE(Documents.Activate(Asset));
	auto& Transactions = *Editor->GetTransactor();
	{
		Durin::FScopedTransaction Recording("Edit Asset");
		ASSERT_TRUE(Asset->Modify(false));
		Package->MarkDirty();
		ASSERT_TRUE(Transactions.StoreUndo(
			std::make_unique<FAppliedPackageEdit>(*Package)));
	}
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Documents.Undo());
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Documents.Redo());
	EXPECT_TRUE(Package->IsDirty());
	const auto UndoLabel = Documents.GetUndoDescription();
	EXPECT_EQ(UndoLabel, "Edit Asset");
	Transactions.Reset();
	EXPECT_TRUE(Documents.GetUndoDescription().empty());
	EXPECT_EQ(UndoLabel, "Edit Asset");

	Durin::MarkObjectHierarchyAsGarbage(Package);
	Durin::MarkAsGarbage(Editor);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FEditorWorkspaceManagerTests, WindowCloseRequestsCanBeDeferredAndRepeated)
{
	Durin::MWindow Window;
	int Requests = 0;
	Window.SetCloseRequestHandler([&Requests] { ++Requests; });
	Window.RequestCloseWindow();
	Window.RequestCloseWindow();
	EXPECT_EQ(Requests, 2);
	// No Mona application exists: reaching destruction instead of the handler would fail.
	Window.SetCloseRequestHandler({});
}

TEST(FEditorWorkspaceUITests, PreservesStableWorkspaceWindowAndDockIdentities)
{
	using namespace Durin;
	const FAssetEditorToolkitId ToolkitId("LevelEditor");

	EXPECT_EQ(TabLayout::MakeMainFrameDockSpaceName(2), "Durin.DockSpace.EditorHost.v2");
	EXPECT_EQ(
		TabLayout::MakeRootWindowName("Level Editor", "LevelEditor"),
		"Level Editor###Durin.Editor.Root.LevelEditor"
	);
	EXPECT_EQ(TabLayout::MakeDockClassName(ToolkitId), "Durin.DockClass.LevelEditor");
	EXPECT_EQ(TabLayout::MakeDockSpaceName(ToolkitId, 4), "Durin.DockSpace.LevelEditor.v4");
	EXPECT_EQ(
		TabLayout::MakePanelWindowName("Details", ToolkitId, "Details"),
		"Details###Durin.LevelEditor.Panel.Details"
	);
	EXPECT_EQ(TabLayout::MakeRootDockClassId(), ImHashStr("Durin.DockClass.EditorRoot"));
	EXPECT_EQ(TabLayout::MakeMainFrameDockSpaceId(2), ImHashStr("Durin.DockSpace.EditorHost.v2"));
	EXPECT_EQ(TabLayout::MakeDockClassId(ToolkitId), ImHashStr("Durin.DockClass.LevelEditor"));
	EXPECT_EQ(TabLayout::MakeDockSpaceId(ToolkitId, 4), ImHashStr("Durin.DockSpace.LevelEditor.v4"));
}

TEST(FEditorWorkspaceUITests, NewWindowRootStartsWithUsableGeometryAndKeepsUserDocking)
{
	using namespace Durin;
	auto* Context = ImGui::CreateContext();
	std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Guard(Context, &ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1600, 1000);
	IO.DeltaTime = 1.0f / 60;
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IO.Fonts->Build();
	FEditorTabWindow Root;
	const auto Name = TabLayout::MakeRootWindowName("Asset", "NewWindowDockRegression");
	const auto DockId = TabLayout::MakeMainFrameDockSpaceId(TabLayout::MainFrameLayoutVersion);
	for (int Frame = 0; Frame < 4; ++Frame)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(1500, 900));
		ImGui::Begin("Dock host");
		TabLayout::SubmitMainFrameDockSpace(TabLayout::MainFrameLayoutVersion, ImVec2(1400, 800));
		ImGui::End();
		if (Frame == 1) ImGui::DockBuilderDockWindow(Name.c_str(), DockId);
		Root.Begin({.DisplayName = "Asset", .RootKey = "NewWindowDockRegression", .OpenLocation = EAssetEditorOpenLocation::NewWindow,
			.AdditionalFlags = ImGuiWindowFlags_NoSavedSettings});
		const auto* Window = ImGui::GetCurrentWindow();
		EXPECT_EQ(0, Window->Flags & ImGuiWindowFlags_NoDocking);
		EXPECT_EQ(TabLayout::MakeRootDockClassId(), Window->WindowClass.ClassId);
		if (Frame == 0)
		{
			EXPECT_EQ(0u, Window->DockId);
			EXPECT_GE(ImGui::GetContentRegionAvail().x, 980.0f);
			EXPECT_GE(ImGui::GetContentRegionAvail().y, 600.0f);
		}
		else EXPECT_EQ(DockId, Window->DockId);
		Root.End();
		ImGui::Render();
		EXPECT_EQ(0, Context->ErrorCountCurrentFrame);
	}
}

TEST(FEditorWorkspaceUITests, DefaultRootDocksInMainWindowAndKeepsUserUndocking)
{
	using namespace Durin;
	auto* Context = ImGui::CreateContext();
	std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Guard(Context, &ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1600, 1000);
	IO.DeltaTime = 1.0f / 60;
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IO.Fonts->Build();
	FEditorTabWindow Root;
	const auto Name = TabLayout::MakeRootWindowName("Asset", "DefaultDockRegression");
	const auto DockId = TabLayout::MakeMainFrameDockSpaceId(TabLayout::MainFrameLayoutVersion);
	for (int Frame = 0; Frame < 4; ++Frame)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(1500, 900));
		ImGui::Begin("Dock host");
		TabLayout::SubmitMainFrameDockSpace(TabLayout::MainFrameLayoutVersion, ImVec2(1400, 800));
		ImGui::End();
		if (Frame == 2) ImGui::SetWindowDock(ImGui::FindWindowByName(Name.c_str()), 0, ImGuiCond_Always);
		Root.Begin({.DisplayName = "Asset", .RootKey = "DefaultDockRegression",
			.AdditionalFlags = ImGuiWindowFlags_NoSavedSettings});
		const auto* Window = ImGui::GetCurrentWindow();
		EXPECT_EQ(Frame < 2 ? DockId : 0u, Window->DockId);
		EXPECT_EQ(0, Window->WindowClass.ViewportFlagsOverrideSet & ImGuiViewportFlags_NoAutoMerge);
		Root.End();
		ImGui::Render();
		EXPECT_EQ(0, Context->ErrorCountCurrentFrame);
	}
}

TEST(FEditorWorkspaceUITests, FocusRequestsSwitchBetweenDockedAndFloatingRoots)
{
	using namespace Durin;
	auto* Context = ImGui::CreateContext();
	std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> Guard(Context, &ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1600, 1000);
	IO.DeltaTime = 1.0f / 60;
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IO.Fonts->Build();
	FEditorTabWindow Docked, Floating;
	const auto DockedName = TabLayout::MakeRootWindowName("Docked", "FocusDocked");
	const auto FloatingName = TabLayout::MakeRootWindowName("Floating", "FocusFloating");
	for (int Frame = 0; Frame < 4; ++Frame)
	{
		if (Frame == 1) Floating.RequestFocus();
		if (Frame == 2) Docked.RequestFocus();
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(1500, 900));
		ImGui::Begin("Dock host");
		TabLayout::SubmitMainFrameDockSpace(TabLayout::MainFrameLayoutVersion, ImVec2(1400, 800));
		ImGui::End();
		Floating.Begin({.DisplayName = "Floating", .RootKey = "FocusFloating", .OpenLocation = EAssetEditorOpenLocation::NewWindow});
		Floating.End();
		Docked.Begin({.DisplayName = "Docked", .RootKey = "FocusDocked"});
		Docked.End();
		ImGui::Render();
		if (Frame > 0)
		{
			ASSERT_NE(nullptr, Context->NavWindow);
			EXPECT_EQ(Frame == 1 ? FloatingName : DockedName, std::string(Context->NavWindow->Name));
		}
		EXPECT_EQ(0, Context->ErrorCountCurrentFrame);
	}
}

TEST(FAssetPickerTests, AppliesExactAndDerivedClassPolicies)
{
	EXPECT_TRUE(Durin::AssetPicker::MatchesClass(
		Durin::DMaterialInstance::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::EAssetClassPolicy::Derived
	));
	EXPECT_FALSE(Durin::AssetPicker::MatchesClass(
		Durin::DMaterialInstance::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::EAssetClassPolicy::Exact
	));
	EXPECT_TRUE(Durin::AssetPicker::MatchesClass(
		Durin::DMaterialInterface::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::EAssetClassPolicy::Exact
	));
	EXPECT_TRUE(Durin::AssetPicker::MatchesClass(
		Durin::DTexture2D::StaticClass(),
		Durin::DTexture::StaticClass(),
		Durin::EAssetClassPolicy::Derived
	));
	EXPECT_TRUE(Durin::AssetPicker::MatchesClass(
		Durin::DTextureCube::StaticClass(),
		Durin::DTexture::StaticClass(),
		Durin::EAssetClassPolicy::Derived
	));
	EXPECT_FALSE(Durin::AssetPicker::MatchesClass(
		Durin::DTexture::StaticClass(),
		Durin::DTexture2D::StaticClass(),
		Durin::EAssetClassPolicy::Derived
	));
}

TEST(FAssetPickerTests, UsesSoftPathForUnloadedCurrentSelection)
{
	EXPECT_EQ(
		Durin::AssetPicker::GetAssetPathOrNone(nullptr, "/Game/Levels/Default.MainLevel", "None"),
		"/Game/Levels/Default.MainLevel"
	);
	EXPECT_EQ(Durin::AssetPicker::GetAssetPathOrNone(nullptr, {}, "None"), "None");
}

TEST(FAssetPickerTests, CanPresentExactSelectionsAsPackagePaths)
{
	EXPECT_EQ(
		Durin::AssetPicker::GetAssetPathDisplayName(
			"/Game/Levels/GrayboxStage15.GrayboxStage15",
			Durin::EAssetPathDisplayMode::PackagePath),
		"/Game/Levels/GrayboxStage15");
	EXPECT_EQ(
		Durin::AssetPicker::GetAssetPathDisplayName(
			"/Game/Levels/GrayboxStage15.GrayboxStage15",
			Durin::EAssetPathDisplayMode::ExactAssetPath),
		"/Game/Levels/GrayboxStage15.GrayboxStage15");
}

TEST(FAssetPickerTests, DisplaysAssetNamesIndependentlyOfPackageNames)
{
	using namespace Durin;
	using namespace Durin;
	EXPECT_EQ(FAssetPickerConfig{}.PathDisplayMode, EAssetPathDisplayMode::AssetName);
	EXPECT_EQ(AssetPicker::GetAssetPathDisplayName(
		"/Game/Props/Furniture.Chair", EAssetPathDisplayMode::AssetName), "Chair");
	EXPECT_EQ(AssetPicker::GetAssetPathDisplayName(
		"/Game/Props/Furniture.Table", EAssetPathDisplayMode::AssetName), "Table");
	EXPECT_EQ(AssetPicker::GetAssetPathDisplayName(
		"/Game/Props/Furniture.Chair", EAssetPathDisplayMode::PackagePath), "/Game/Props/Furniture");
	EXPECT_EQ(AssetPicker::GetAssetPathDisplayName(
		"/Game/Props/Furniture.Chair", EAssetPathDisplayMode::ExactAssetPath), "/Game/Props/Furniture.Chair");
	EXPECT_TRUE(AssetPicker::GetAssetPathDisplayName({}, EAssetPathDisplayMode::AssetName).empty());
}

TEST(FAssetPickerTests, KeepsLoadedAssetsInTheSamePackageDistinct)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	Testing::FScopedMountRegistryFixture MountFixture;
	Testing::RegisterMountPointForTests(
		"/AssetPickerTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetPickerTests/Furniture", PackagePath));
	DPackage* Package = NewObject<DPackage>(nullptr, "Furniture");
	ASSERT_NE(Package, nullptr);
	Package->InitializeAssetPackage(PackagePath);
	DObject* Chair = NewObject<DObject>(Package, "Chair");
	DObject* Table = NewObject<DObject>(Package, "Table");
	ASSERT_NE(Chair, nullptr);
	ASSERT_NE(Table, nullptr);
	EXPECT_EQ(AssetPicker::GetAssetPathOrNone(Chair), "/AssetPickerTests/Furniture.Chair");
	EXPECT_EQ(AssetPicker::GetAssetPathOrNone(Table), "/AssetPickerTests/Furniture.Table");
	EXPECT_EQ(AssetPicker::GetAssetPathOrNone(Chair, {}, "None"), Chair->GetObjectPath());
	EXPECT_EQ(AssetPicker::GetAssetPathOrNone(nullptr), "None");
	MarkObjectHierarchyAsGarbage(Package);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FAssetPickerTests, OpeningMaterialCandidatesCompletesPopupLayout)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	Testing::FScopedMountRegistryFixture MountFixture;
	Testing::RegisterMountPointForTests(
		"/AssetPickerLayoutTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath PackagePath;
	FTopLevelAssetPath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetPickerLayoutTests/Materials", PackagePath));
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(PackagePath, "Material", AssetPath));
	auto Original = CaptureAssetRegistryPublication();
	auto Publication = Original;
	Publication.Assets.emplace(PackagePath, FPackageData{
		.PackagePath = PackagePath,
		.TopLevelAssets = {{.AssetPath = AssetPath, .AssetClassName = "Durin::DMaterial"}},
		.FormatVersion = 11, .ObjectCount = 1});
	Publication.ReferenceFingerprints.emplace(PackagePath, FAssetPackageFingerprint{.ReaderVersion = 11});
	ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
	auto& ThumbnailManager = GetDefaultThumbnailManager();
	auto Registration = Durin::FThumbnailManagerTestAccess::RegisterScoped(ThumbnailManager,
		Durin::MakeThumbnailRenderer<FPickerThumbnailRenderer>(), "Durin::DMaterial");
	ASSERT_TRUE(Registration);
	auto& ThumbnailPool = ThumbnailManager.GetSharedPool();
	ImGuiContext* Context = ImGui::CreateContext();
	ImGuiIO& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(800.0f, 600.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	IO.ConfigErrorRecoveryEnableAssert = false;
	std::array<char, 64> Search{};
	for (int Frame = 0; Frame < 3; ++Frame)
	{
		ThumbnailPool.Tick(false);
		if (Frame > 1)
			EXPECT_EQ(ThumbnailPool.Find(AssetPath).State, EAssetThumbnailState::Pending);
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
		ImGui::Begin("AssetPickerLayoutTest");
		ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID("##MaterialPicker")), 0);
		const auto Result = AssetPicker::Draw({
			.ComboId = "##MaterialPicker", .RequiredClass = DMaterial::StaticClass(),
			.SearchText = Search,
			.AssignSelection = [](DObject*, std::string&) { return true; },
			.PathPrefixFilter = "/AssetPickerLayoutTests/"});
		EXPECT_TRUE(Result.Error.empty());
		EXPECT_FALSE(Result.bSelectionChanged);
		ImGui::End();
		ImGui::Render();
		EXPECT_EQ(Context->ErrorCountCurrentFrame, 0);
		// ImGui uses the first popup frame to measure its auto-fit layout;
		// visible rows start submitting requests on the following frame.
		if (Frame > 0)
		{
			EXPECT_EQ(ThumbnailPool.Find(AssetPath).State, EAssetThumbnailState::Pending);
			EXPECT_EQ(ThumbnailPool.GetStats().QueuedJobs, 1u);
		}
		EXPECT_EQ(ThumbnailPool.GetStats().Referencers, 0u);
	}
	EXPECT_EQ(GetDefaultThumbnailManager().GetSharedPool().GetStats().RetainedEntries, 1u);
	ImGui::DestroyContext(Context);
	GetDefaultThumbnailManager().ResetSharedPool();
	Original.ExpectedRevision = GetAssetCatalogRevision();
	EXPECT_TRUE(PublishAssetRegistryPublication(std::move(Original)));
}

TEST(FAssetPickerTests, FiltersCandidatesByPathPrefix)
{
	EXPECT_TRUE(Durin::AssetPicker::MatchesPathPrefix("/Engine/Materials/Default", {}));
	EXPECT_TRUE(Durin::AssetPicker::MatchesPathPrefix("/Game/Levels/Default", "/Game/"));
	EXPECT_TRUE(Durin::AssetPicker::MatchesPathPrefix("/Game/Levels/Default", "/Game/Levels/"));
	EXPECT_FALSE(Durin::AssetPicker::MatchesPathPrefix("/Engine/Levels/Default", "/Game/"));
	EXPECT_FALSE(Durin::AssetPicker::MatchesPathPrefix("/Gameplay/Levels/Default", "/Game/"));
}

// Exercise real ImGui modal lifetime across host frames, including cancellation.
TEST(FSlowTaskDialogTests, ShowsProgressBlocksInputAndClosesBeforeTheNextLoad)
{
	using namespace Durin;
	ImGuiContext* Context = ImGui::CreateContext();
	struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
	ImGuiIO& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1000.0f, 700.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	FFeedbackContext Feedback;
	FSlowTaskDialogPresenter Presenter;
	auto Task = std::make_unique<FSlowTask>(0, "Opening Target...", Feedback);
	Task->MakeDialog(true);
	ImVec2 CancelPosition;
	auto Frame = [&]() {
		ImGui::NewFrame();
		ImGui::Begin("LoadingDialogHost");
		const bool bCancel = Presenter.Draw(Feedback);
		if (Task)
		{
			EXPECT_NE(ImGui::GetTopMostPopupModal(), nullptr);
			const ImGuiWindow* Modal = ImGui::GetTopMostPopupModal();
			const ImVec2 ButtonSize(ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f,
				ImGui::GetFrameHeight());
			CancelPosition = ImVec2(Modal->DC.CursorPosPrevLine.x - ButtonSize.x * 0.5f,
				Modal->DC.CursorPosPrevLine.y + ButtonSize.y * 0.5f);
		}
		else
			EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
		ImGui::End();
		ImGui::Render();
		return bCancel;
	};
	EXPECT_FALSE(Frame());
	Task->SetMessage("Preparing geometry and materials");
	EXPECT_FALSE(Frame());
	EXPECT_FALSE(Frame());
	EXPECT_TRUE(IO.WantCaptureMouse);
	IO.AddMousePosEvent(CancelPosition.x, CancelPosition.y);
	EXPECT_FALSE(Frame());
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	EXPECT_FALSE(Frame());
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	EXPECT_TRUE(Frame());
	Task.reset();
	EXPECT_FALSE(Frame());
	Task = std::make_unique<FSlowTask>(1, "Second load", Feedback);
	Task->MakeDialog(true);
	Task->SetProgress(0.5);
	EXPECT_FALSE(Frame());
	Task.reset();
	EXPECT_FALSE(Frame());
}

TEST(FSlowTaskDialogTests, IdlePresenterDoesNotOpenAModal)
{
	ImGuiContext* Context = ImGui::CreateContext();
	struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1000.0f, 700.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	ImGui::NewFrame();
	ImGui::Begin("LoadingDialogHost");
	Durin::FFeedbackContext Feedback;
	Durin::FSlowTaskDialogPresenter Presenter;
	EXPECT_FALSE(Presenter.Draw(Feedback));
	EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
	ImGui::End();
	ImGui::Render();
}

TEST(FSlowTaskDialogTests, DelaysPresentationAndClosesOnCompletionWithALiveTask)
{
	ImGuiContext* Context = ImGui::CreateContext();
	struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1000.0f, 700.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	double Now = 0;
	Durin::FFeedbackContext Feedback([&] { return Now; });
	Durin::FSlowTask Task(100, "Preparing workspace", Feedback);
	Durin::FSlowTaskDialogPresenter Presenter;
	Task.MakeDialogDelayed(0.25);
	auto Frame = [&](bool bExpectedModal) {
		ImGui::NewFrame();
		ImGui::Begin("SlowTaskHost");
		EXPECT_FALSE(Presenter.Draw(Feedback));
		EXPECT_EQ(ImGui::GetTopMostPopupModal() != nullptr, bExpectedModal);
		ImGui::End();
		ImGui::Render();
	};
	Frame(false);
	Now = 0.25;
	Task.SetProgress(0.5, "Loading settings");
	Frame(true);
	Task.Complete();
	Frame(false);
	EXPECT_EQ(Task.GetSnapshot()->Status, Durin::ESlowTaskStatus::Completed);
	// Startup presentation uses the same snapshot without opening a modal.
	ImGui::NewFrame();
	ImGui::Begin("StartupHost");
	EXPECT_FALSE(Durin::DrawSlowTaskProgress(*Task.GetSnapshot()));
	EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
	ImGui::End();
	ImGui::Render();
}

TEST(FSlowTaskDialogTests, InlineSnapshotRoutesCancelAndCanHideUnavailableTiming)
{
	ImGuiContext* Context = ImGui::CreateContext();
	struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1000.0f, 700.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	Durin::FSlowTaskSnapshot Progress{.Title = "Importing Scene", .Message = "Preparing assets", .bCanCancel = true};
	ImVec2 CancelPosition;
	auto Frame = [&]() {
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(440.0f, 220.0f));
		ImGui::Begin("InlineProgress");
		const bool bClicked = Durin::DrawSlowTaskProgress(Progress, true, false);
		CancelPosition = Context->LastItemData.Rect.GetCenter();
		EXPECT_EQ((Context->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0, Progress.bCancelRequested);
		EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
		ImGui::End();
		ImGui::Render();
		return bClicked;
	};
	EXPECT_FALSE(Frame());
	EXPECT_FALSE(Frame());
	IO.AddMousePosEvent(CancelPosition.x, CancelPosition.y);
	EXPECT_FALSE(Frame());
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	EXPECT_FALSE(Frame());
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	EXPECT_TRUE(Frame());
	Progress.bCancelRequested = true;
	EXPECT_FALSE(Frame());
}

TEST(FSlowTaskDialogTests, StartupProgressWithoutFooterDoesNotExtendWindowWithAnEmptyCursorMove)
{
	ImGuiContext* Context = ImGui::CreateContext();
	struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1000.0f, 700.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	ImGui::NewFrame();
	ImGui::Begin("StartupProgress");
	ImGui::SetCursorPos(ImVec2(320.0f, 250.0f));
	EXPECT_FALSE(Durin::DrawSlowTaskProgress({.Message = "Starting editor..."}, false, false));
	EXPECT_FALSE(ImGui::GetCurrentWindow()->DC.IsSetPos);
	ImGui::End();
	ImGui::Render();
}

TEST(FSlowTaskDialogTests, ProjectedModalReturnsCancelAndClosesWithoutTaskState)
{
	using namespace Durin;
	ImGuiContext* Context = ImGui::CreateContext();
	struct FCleanup { ImGuiContext* Context; ~FCleanup() { ImGui::DestroyContext(Context); } } Cleanup{Context};
	auto& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.DisplaySize = ImVec2(1000.0f, 700.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();
	FSlowTaskDialogPresenter Presenter;
	std::optional<FSlowTaskSnapshot> Feedback = FSlowTaskSnapshot{
		.Title = "Opening Target...", .Message = "Loading assets", .ElapsedSeconds = 1.25, .bCanCancel = true};
	ImVec2 CancelPosition;
	auto Frame = [&]() {
		ImGui::NewFrame();
		ImGui::Begin("ProjectedLoadingHost");
		const bool bCancel = Presenter.Draw(Feedback);
		const bool bExpectedModal = Feedback && Feedback->Status == ESlowTaskStatus::Running;
		const ImGuiWindow* Modal = ImGui::GetTopMostPopupModal();
		EXPECT_EQ(Modal != nullptr, bExpectedModal);
		if (Modal)
			CancelPosition = ImVec2(Modal->DC.CursorPosPrevLine.x - MonaImGui::ScaleUI(35.0f),
				Modal->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5f);
		ImGui::End();
		ImGui::Render();
		return bCancel;
	};
	EXPECT_FALSE(Frame());
	EXPECT_FALSE(Frame());
	IO.AddMousePosEvent(CancelPosition.x, CancelPosition.y);
	EXPECT_FALSE(Frame());
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	EXPECT_FALSE(Frame());
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	EXPECT_TRUE(Frame());
	EXPECT_FALSE(Feedback->bCancelRequested);
	Feedback.reset();
	EXPECT_FALSE(Frame());
	Feedback = FSlowTaskSnapshot{.Status = ESlowTaskStatus::Failed};
	EXPECT_FALSE(Frame());
	Feedback = FSlowTaskSnapshot{.Title = "Next level", .Message = "Preparing initial view"};
	EXPECT_FALSE(Frame());
	Feedback->Status = ESlowTaskStatus::Completed;
	EXPECT_FALSE(Frame());
}
