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
#include "Documents/EditorDocumentManager.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "Framework/TabLayout.h"
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
		auto GetRegistration() const -> Durin::FThumbnailRenderingInfo override
		{
			return {"Durin::DMaterial", "PickerThumbnailTest", 1};
		}
		auto DescribeThumbnail(const Durin::FAssetThumbnailPackageFingerprint& Asset) -> std::expected<Durin::FThumbnailCacheDescription, std::string> override
		{
			ADD_FAILURE() << "This test must not execute thumbnail generation.";
			return std::unexpected("Unexpected thumbnail generation.");
		}
		auto CanVisualizeAsset(Durin::DObject*) const -> bool override { return true; }
		auto Draw(Durin::DObject*, Durin::FThumbnailDrawContext&) -> void override { ADD_FAILURE() << "Unexpected drawing."; }

	};

	class FAppliedPackageEdit final : public Durin::FActorCommandChange
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
		auto Replay(::Durin::ETransactionOperation Operation) -> Durin::FActorChangeResult override
		{
			if (Operation == ::Durin::ETransactionOperation::Undo)
			{
				return {};
			}
			else
			{
				return {};
			}
		}

	private:
		std::array<Durin::DPackage*, 1> Package{};
	};

	class FTestWorkspace final : public Durin::IAssetEditorToolkit
	{
	public:
		explicit FTestWorkspace(std::string Type)
			: ToolkitId(std::move(Type))
		{
		}

		auto GetToolkitId() const -> const Durin::FAssetEditorToolkitId& override { return ToolkitId; }
		auto OpenDocument(const Durin::FEditorDocument&) -> Durin::EDocumentOpenResult override
		{
			return OpenResult;
		}
		auto ActivateDocument(const Durin::FEditorDocument& Document) -> void override
		{
			++ActivationCount;
			LastActivatedResource = Document.ResourceId;
		}
		auto RequestDeactivate() -> bool override
		{
			++DeactivationRequestCount;
			return bAllowDeactivation;
		}
		auto RequestCloseDocument(const Durin::FEditorDocument&) -> Durin::EDocumentCloseResult override
		{
			++CloseRequestCount;
			return CloseResult;
		}
		auto SaveDocument(const Durin::FEditorDocument& Document) -> bool override
		{
			++SaveCount;
			LastSavedResource = Document.ResourceId;
			if (bAllowSave)
			{
				CloseResult = Durin::EDocumentCloseResult::Closed;
				DirtyDocuments.erase(Document.Id.Value);
			}
			return bAllowSave;
		}
		auto DiscardDocument(const Durin::FEditorDocument& Document) -> bool override
		{
			++DiscardCount;
			LastDiscardedResource = Document.ResourceId;
			if (bAllowDiscard) CloseResult = Durin::EDocumentCloseResult::Closed;
			return bAllowDiscard;
		}
		auto DrawToolkit(bool) -> bool override { return false; }
		auto IsDocumentDirty(const Durin::FEditorDocument& Document) const -> bool override
		{
			return bTrackDirtyDocuments ? DirtyDocuments.contains(Document.Id.Value) : Document.bDirty;
		}
		auto ResetLayout() -> void override {}

		int ActivationCount = 0;
		std::unordered_set<uint64_t> DirtyDocuments;
		bool bTrackDirtyDocuments = false;
		int DeactivationRequestCount = 0;
		int CloseRequestCount = 0;
		int SaveCount = 0;
		int DiscardCount = 0;
		bool bAllowDeactivation = true;
		bool bAllowSave = true;
		bool bAllowDiscard = true;
		Durin::EDocumentOpenResult OpenResult = Durin::EDocumentOpenResult::Opened;
		Durin::EDocumentCloseResult CloseResult = Durin::EDocumentCloseResult::Closed;
		std::string LastActivatedResource;
		std::string LastSavedResource;
		std::string LastDiscardedResource;

	private:
		Durin::FAssetEditorToolkitId ToolkitId;
	};

	auto OpenResource(Durin::FEditorDocumentManager& Manager, const std::shared_ptr<FTestWorkspace>& Toolkit,
		std::string Path) -> Durin::FDocumentId
	{
		const auto Leaf = Path.substr(Path.find_last_of('/') + 1);
		return Manager.OpenDocument({.ToolkitId = Toolkit->GetToolkitId(), .DocumentKey = Path,
			.ResourceId = Path, .Label = Leaf});
	}

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
	Editor->DispatchSubsystemCallback([&] {
		Durin::FScopedTransaction Missing(nullptr, {.Description = "No transaction owner"});
		EXPECT_FALSE(Missing.IsOutstanding());
		EXPECT_FALSE(Buffer->IsActive());
	});
	{
		Durin::FScopedTransaction Scope(Editor->GetTransactor(), {.Description = "Editor transaction"});
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
	auto Capture = Durin::FTransactionObjectRecord::CaptureWhole(*Editor);
	ASSERT_TRUE(Capture) << Durin::FormatTransactionObjectRecordError(Capture.error());
	ASSERT_TRUE(Durin::FTransBufferTestAccess::Record(*Buffer, std::move(*Capture)));
	Editor->BeginDestroy();
	EXPECT_EQ(Editor->GetTransactor(), nullptr);
	EXPECT_EQ(Buffer->GetState(), Durin::ETransactorState::Destroying);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Buffer->GetUndoSize(), 0u);
}

TEST(FEditableAssetDocumentModelTests, UndoToActivatedRevisionClearsDirtyState)
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

	Durin::FEditableAssetDocumentModel Documents;
	const Durin::FEditorDocument Document{
		.Id = {1},
		.ResourceId = "/EditableAssetDocumentTests/UndoCheckpoint",
	};
	ASSERT_TRUE(Documents.Activate(Document, Asset));
	Durin::DTransactor& Transactions =
		*Editor->GetTransactor();
	const auto OpenRevision = Transactions.GetPackageRevisionState(*Package);
	ASSERT_TRUE(OpenRevision.has_value());
	EXPECT_EQ(OpenRevision->CurrentRevision, OpenRevision->SavedRevision);

	{
		Durin::FScopedTransaction Recording(&Transactions, {.Description = "Edit Asset"});
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
	ASSERT_TRUE(Transactions.TryReset());
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

TEST(FEditorWorkspaceManagerTests, ExitSavesAllDirtyDocumentsWithoutClosingTabs)
{
	using namespace Durin;
	using namespace Durin;
	FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("ExitTest");
	Workspace->bTrackDirtyDocuments = true;
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	const auto First = Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
		.DocumentKey = "first", .Label = "First", .bClosable = false});
	const auto Second = Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
		.DocumentKey = "second", .Label = "Second"});
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Second.IsValid());
	Workspace->DirtyDocuments = {First.Value, Second.Value};
	ASSERT_TRUE(Manager.PrepareForExit());
	EXPECT_TRUE(Manager.GetDocuments().front().bDirty);
	EXPECT_EQ(Workspace->SaveCount, 0);
	EXPECT_EQ(Workspace->CloseRequestCount, 0);
	Workspace->bAllowSave = false;
	EXPECT_FALSE(Manager.SaveDocumentsForExit());
	EXPECT_EQ(Manager.GetDocuments().size(), 2);
	EXPECT_EQ(Workspace->DirtyDocuments.size(), 2);
	Workspace->bAllowSave = true;
	EXPECT_TRUE(Manager.SaveDocumentsForExit());
	EXPECT_TRUE(Workspace->DirtyDocuments.empty());
	EXPECT_EQ(Manager.GetDocuments().size(), 2);
	EXPECT_EQ(Manager.GetActiveDocument()->Id, Second);
	EXPECT_EQ(Workspace->CloseRequestCount, 0);
}

TEST(FEditorWorkspaceManagerTests, ExitRetainsAllWorkspacesWhenALaterSaveFails)
{
	using namespace Durin;
	using namespace Durin;
	FEditorDocumentManager Manager, Other;
	auto First = std::make_shared<FTestWorkspace>("First");
	auto Second = std::make_shared<FTestWorkspace>("Second");
	First->bTrackDirtyDocuments = Second->bTrackDirtyDocuments = true;
	ASSERT_TRUE(Manager.AttachSessionToolkit(First, {}));
	ASSERT_TRUE(Other.AttachSessionToolkit(Second, {}));
	const auto FirstId = Manager.OpenDocument({.ToolkitId = First->GetToolkitId(),
		.DocumentKey = "first", .Label = "First"});
	const auto SecondId = Other.OpenDocument({.ToolkitId = Second->GetToolkitId(),
		.DocumentKey = "second", .Label = "Second"});
	First->DirtyDocuments.insert(FirstId.Value);
	Second->DirtyDocuments.insert(SecondId.Value);
	Second->bAllowSave = false;
	EXPECT_TRUE(Manager.SaveDocumentsForExit());
	EXPECT_FALSE(Other.SaveDocumentsForExit());
	EXPECT_TRUE(First->DirtyDocuments.empty());
	EXPECT_FALSE(Second->DirtyDocuments.empty());
	EXPECT_EQ(Manager.GetDocuments().size() + Other.GetDocuments().size(), 2);
	Second->bAllowSave = true;
	EXPECT_TRUE(Other.SaveDocumentsForExit());
	EXPECT_EQ(First->SaveCount, 1);
	EXPECT_EQ(Second->SaveCount, 2);
	EXPECT_EQ(Manager.GetDocuments().size() + Other.GetDocuments().size(), 2);
}

TEST(FEditorWorkspaceManagerTests, ExitVetoesUnsettledPreviewsAndPendingDocuments)
{
	using namespace Durin;
	using namespace Durin;
	FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("ExitTest");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	const auto Document = Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
		.DocumentKey = "first", .Label = "First"});
	Workspace->bAllowDeactivation = false;
	EXPECT_FALSE(Manager.PrepareForExit());
	EXPECT_FALSE(Manager.SaveDocumentsForExit());
	Workspace->bAllowDeactivation = true;
	Workspace->CloseResult = EDocumentCloseResult::PendingConfirmation;
	ASSERT_EQ(Manager.RequestCloseDocument(Document), EDocumentCloseResult::PendingConfirmation);
	EXPECT_FALSE(Manager.PrepareForExit());
	Manager.ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel);
	EXPECT_TRUE(Manager.PrepareForExit());
	Workspace->OpenResult = EDocumentOpenResult::Deferred;
	const auto Deferred = Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
		.DocumentKey = "deferred", .Label = "Deferred"});
	EXPECT_FALSE(Manager.PrepareForExit());
	Manager.CompleteDeferredDocumentOpen(Deferred, false);
	EXPECT_TRUE(Manager.PrepareForExit());
	EXPECT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Workspace->SaveCount, 0);
}

TEST(FEditorWorkspaceManagerTests, ReportsDocumentVisibilityBeforeDrawing)
{
	ImGuiContext* Context = ImGui::CreateContext();
	ASSERT_NE(Context, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.IniFilename = nullptr;
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IO.DisplaySize = ImVec2(1280.0f, 720.0f);
	IO.DeltaTime = 1.0f / 60.0f;
	IO.Fonts->Build();

	Durin::FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	ASSERT_TRUE(Attached);
	ASSERT_TRUE(OpenResource(Manager, Workspace, "/Game/Materials/M_Visible").IsValid());

	std::vector<std::string> Events;
	ImGui::NewFrame();
	Durin::FTabManager DocumentHost;
	DocumentHost.DrawDocuments(
		Manager,
		Durin::FAssetEditorToolkitId("MaterialEditor"),
		"MaterialEditor",
		[](const Durin::FEditorDocument&) { return true; },
		[&Events](const Durin::FEditorDocument&) { Events.emplace_back("draw"); },
		[&Events](const Durin::FEditorDocument&, bool bVisible) {
			Events.emplace_back(bVisible ? "visible" : "hidden");
		});
	ImGui::Render();
	ImGui::DestroyContext(Context);

	EXPECT_EQ(Events, (std::vector<std::string>{"visible", "draw"}));
}

TEST(FEditorWorkspaceManagerTests, OpensAndSwitchesMultiplePerResourceDocumentsInOneWorkspace)
{
	Durin::FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	ASSERT_TRUE(Attached);
	ASSERT_TRUE(OpenResource(Manager, Workspace, "/Game/Materials/M_Stone").IsValid());
	ASSERT_TRUE(OpenResource(Manager, Workspace, "/Game/Materials/MI_Stone").IsValid());
	ASSERT_EQ(Manager.GetDocuments().size(), 2);
	EXPECT_EQ(Manager.GetDocuments()[0].Label, "M_Stone");
	EXPECT_EQ(Manager.GetDocuments()[1].Label, "MI_Stone");
	EXPECT_EQ(Workspace->LastActivatedResource, "/Game/Materials/MI_Stone");

	EXPECT_TRUE(Manager.ActivateDocument(Manager.GetDocuments()[0].Id));
	EXPECT_EQ(Workspace->LastActivatedResource, "/Game/Materials/M_Stone");
}

TEST(FEditorWorkspaceManagerTests, OpenAssetDocumentsRetainEveryAssetFamilyAndBlockRemovalUntilClosed)
{
	using namespace Durin;
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/OpenDocumentTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
	const std::array Classes{DMaterial::StaticClass(), DMaterialInstance::StaticClass(),
		DTexture2D::StaticClass(), DVolumeTexture::StaticClass(), DStaticMesh::StaticClass(),
		DMaterialFunction::StaticClass(), DMaterialParameterCollection::StaticClass()};
	for (size_t Index = 0; Index < Classes.size(); ++Index)
	{
		SCOPED_TRACE(Classes[Index]->GetQualifiedName().ToString());
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/OpenDocumentTests/Asset{}", Index), Path));
		auto* Package = CreatePackage(Path);
		ASSERT_NE(Package, nullptr);
		auto* Asset = NewObject(Classes[Index], Package, FName(std::format("Asset{}", Index)));
		ASSERT_NE(Asset, nullptr);
		Package->ClearDirty();
		const FObjectKey Key(Asset);
		const std::array Paths{Path};
		EXPECT_TRUE(ValidatePackageRemovalReferences(Paths));
		FEditorDocumentManager Manager;
		auto Workspace = std::make_shared<FTestWorkspace>("AssetEditor");
		const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
		const auto Id = Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
			.DocumentKey = Asset->GetObjectPath(), .ResourceId = Asset->GetObjectPath(), .Label = "Asset"});
		ASSERT_TRUE(Id.IsValid());
		// A different active tab must not release the first document's ownership.
		ASSERT_TRUE(Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
			.DocumentKey = "Other", .Label = "Other"}).IsValid());
		EXPECT_EQ(ValidatePackageRemovalReferences(Paths).Error, EAssetWriteError::InUse);
		CollectGarbage(Durin::EObjectFlags::Standalone);
		ASSERT_NE(Key.ResolveObjectPtr(), nullptr);
		Workspace->CloseResult = EDocumentCloseResult::PendingConfirmation;
		EXPECT_EQ(Manager.RequestCloseDocument(Id), EDocumentCloseResult::PendingConfirmation);
		EXPECT_EQ(Manager.ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel), EDocumentCloseResult::Cancelled);
		EXPECT_EQ(ValidatePackageRemovalReferences(Paths).Error, EAssetWriteError::InUse);
		const auto SharedId = Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
			.DocumentKey = "Shared", .ResourceId = Asset->GetObjectPath(), .Label = "Shared asset"});
		ASSERT_TRUE(SharedId.IsValid());
		Workspace->CloseResult = EDocumentCloseResult::Closed;
		EXPECT_EQ(Manager.RequestCloseDocument(Id), EDocumentCloseResult::Closed);
		EXPECT_EQ(ValidatePackageRemovalReferences(Paths).Error, EAssetWriteError::InUse);
		EXPECT_EQ(Manager.RequestCloseDocument(SharedId), EDocumentCloseResult::Closed);
		EXPECT_TRUE(ValidatePackageRemovalReferences(Paths));
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(Key.ResolveObjectPtr(), nullptr);
	}
}

TEST(FEditorWorkspaceManagerTests, DestroyingControllerReleasesOpenDocumentRootsWithoutSubsystem)
{
	using namespace Durin;
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/UnregisterDocumentTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/UnregisterDocumentTests/Asset", Path));
	auto* Package = CreatePackage(Path);
	auto* Asset = NewObject<DTexture2D>(Package, "Asset");
	Package->ClearDirty();
	const std::array Paths{Path};
	auto OwnedManager = std::make_unique<FEditorDocumentManager>();
	auto& Manager = *OwnedManager;
	auto Workspace = std::make_shared<FTestWorkspace>("AssetEditor");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	ASSERT_TRUE(Manager.OpenDocument({.ToolkitId = Workspace->GetToolkitId(),
		.DocumentKey = Asset->GetObjectPath(), .ResourceId = Asset->GetObjectPath(), .Label = "Asset"}).IsValid());
	EXPECT_EQ(ValidatePackageRemovalReferences(Paths).Error, EAssetWriteError::InUse);
	OwnedManager.reset();
	EXPECT_TRUE(ValidatePackageRemovalReferences(Paths));
	MarkObjectHierarchyAsGarbage(Package);
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FEditorWorkspaceManagerTests, RemapsOpenPerResourceDocumentsAfterAssetRelocation)
{
	Durin::FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	ASSERT_TRUE(Attached);
	ASSERT_TRUE(OpenResource(Manager, Workspace, "/Game/Materials/Old/M_Stone").IsValid());
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	const Durin::FDocumentId DocumentId = Manager.GetDocuments().front().Id;

	Manager.RemapResourceId(
		"/Game/Materials/Old/M_Stone", "/Game/Materials/New/M_Granite");
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	const Durin::FEditorDocument& Document = Manager.GetDocuments().front();
	EXPECT_EQ(Document.Id, DocumentId);
	EXPECT_EQ(Document.ResourceId, "/Game/Materials/New/M_Granite");
	EXPECT_EQ(Document.DocumentKey, "/Game/Materials/New/M_Granite");
	EXPECT_EQ(Document.Label, "M_Granite");
	EXPECT_TRUE(Manager.ActivateDocument(DocumentId));
	EXPECT_EQ(Workspace->LastActivatedResource,
		"/Game/Materials/New/M_Granite");
}

TEST(FEditorWorkspaceManagerTests, CommitsDeferredSingletonReplacementOnlyAfterCompletion)
{
	Durin::FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("LevelEditor");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	ASSERT_TRUE(Attached);

	const Durin::FDocumentId DocumentId = Manager.OpenDocument({
		.ToolkitId = Durin::FAssetEditorToolkitId("LevelEditor"),
		.DocumentKey = "LevelEditor",
		.ResourceId = "/Game/Maps/First",
		.Label = "First",
	});
	ASSERT_TRUE(DocumentId.IsValid());
	ASSERT_EQ(Manager.GetDocuments().size(), 1);

	Workspace->OpenResult = Durin::EDocumentOpenResult::Deferred;
	const Durin::FDocumentId DeferredId = Manager.OpenDocument({
		.ToolkitId = Durin::FAssetEditorToolkitId("LevelEditor"),
		.DocumentKey = "LevelEditor",
		.ResourceId = "/Game/Maps/Second",
		.Label = "Second",
	});
	EXPECT_EQ(DeferredId, DocumentId);
	EXPECT_EQ(Manager.GetDocuments().front().ResourceId, "/Game/Maps/First");
	EXPECT_EQ(Manager.GetDocuments().front().Label, "First");

	EXPECT_TRUE(Manager.CompleteDeferredDocumentOpen(DeferredId, false));
	EXPECT_EQ(Manager.GetDocuments().front().ResourceId, "/Game/Maps/First");
	EXPECT_EQ(Workspace->LastActivatedResource, "/Game/Maps/First");

	const Durin::FDocumentId SecondDeferredId = Manager.OpenDocument({
		.ToolkitId = Durin::FAssetEditorToolkitId("LevelEditor"),
		.DocumentKey = "LevelEditor",
		.ResourceId = "/Game/Maps/Second",
		.Label = "Second",
	});
	ASSERT_EQ(SecondDeferredId, DocumentId);
	EXPECT_TRUE(Manager.CompleteDeferredDocumentOpen(DocumentId, true));
	EXPECT_EQ(Manager.GetDocuments().front().ResourceId, "/Game/Maps/Second");
	EXPECT_EQ(Manager.GetDocuments().front().Label, "Second");
	EXPECT_EQ(Workspace->LastActivatedResource, "/Game/Maps/Second");
}

TEST(FEditorWorkspaceManagerTests, KeepsActiveDocumentWhenHostCannotRestoreItsPreview)
{
	Durin::FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	ASSERT_TRUE(Attached);
	ASSERT_TRUE(OpenResource(Manager, Workspace, "/Game/Materials/M_Stone").IsValid());
	ASSERT_TRUE(OpenResource(Manager, Workspace, "/Game/Materials/MI_Stone").IsValid());
	const Durin::FDocumentId First = Manager.GetDocuments()[0].Id;
	const Durin::FDocumentId Second = Manager.GetDocuments()[1].Id;
	ASSERT_EQ(Manager.GetActiveDocument()->Id, Second);

	Workspace->bAllowDeactivation = false;
	EXPECT_FALSE(Manager.ActivateDocument(First));
	Workspace->bAllowDeactivation = true;
	ASSERT_NE(Manager.GetActiveDocument(), nullptr);
	EXPECT_EQ(Manager.GetActiveDocument()->Id, Second);
	EXPECT_EQ(Workspace->LastActivatedResource, "/Game/Materials/MI_Stone");
	EXPECT_EQ(Workspace->DeactivationRequestCount, 2);
}

TEST(FEditorWorkspaceManagerTests, CoordinatesPendingDocumentCloseResponses)
{
	Durin::FEditorDocumentManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	const bool Attached = Manager.AttachSessionToolkit(Workspace, {});
	ASSERT_TRUE(Attached);

	const Durin::FDocumentId First = Manager.OpenDocument({
		.ToolkitId = Durin::FAssetEditorToolkitId("MaterialEditor"),
		.DocumentKey = "First",
		.ResourceId = "/Game/Materials/M_First",
		.Label = "M_First",
	});
	const Durin::FDocumentId Second = Manager.OpenDocument({
		.ToolkitId = Durin::FAssetEditorToolkitId("MaterialEditor"),
		.DocumentKey = "Second",
		.ResourceId = "/Game/Materials/M_Second",
		.Label = "M_Second",
	});
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Second.IsValid());

	Workspace->CloseResult = Durin::EDocumentCloseResult::PendingConfirmation;
	EXPECT_EQ(Manager.RequestCloseDocument(First), Durin::EDocumentCloseResult::PendingConfirmation);
	ASSERT_NE(Manager.GetPendingCloseDocument(), nullptr);
	EXPECT_EQ(Manager.GetPendingCloseDocument()->Id, First);
	EXPECT_EQ(Manager.RequestCloseDocument(First), Durin::EDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::EDocumentCloseResult::Rejected);

	Workspace->bAllowSave = false;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EDocumentCloseResponse::Save),
		Durin::EDocumentCloseResult::PendingConfirmation
	);
	EXPECT_EQ(Manager.GetDocuments().size(), 2);
	ASSERT_NE(Manager.GetPendingCloseDocument(), nullptr);

	Workspace->bAllowSave = true;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EDocumentCloseResponse::Save),
		Durin::EDocumentCloseResult::Closed
	);
	EXPECT_EQ(Workspace->LastSavedResource, "/Game/Materials/M_First");
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().Id, Second);
	EXPECT_EQ(Manager.GetPendingCloseDocument(), nullptr);

	Workspace->CloseResult = Durin::EDocumentCloseResult::PendingConfirmation;
	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::EDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EDocumentCloseResponse::Cancel),
		Durin::EDocumentCloseResult::Cancelled
	);
	EXPECT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetPendingCloseDocument(), nullptr);

	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::EDocumentCloseResult::PendingConfirmation);
	Workspace->bAllowDiscard = false;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EDocumentCloseResponse::Discard),
		Durin::EDocumentCloseResult::PendingConfirmation
	);
	ASSERT_NE(Manager.GetPendingCloseDocument(), nullptr);
	EXPECT_EQ(Manager.GetPendingCloseDocument()->Id, Second);
	EXPECT_EQ(Manager.GetDocuments().size(), 1);
	Workspace->bAllowDiscard = true;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EDocumentCloseResponse::Discard),
		Durin::EDocumentCloseResult::Closed
	);
	EXPECT_EQ(Workspace->LastDiscardedResource, "/Game/Materials/M_Second");
	EXPECT_TRUE(Manager.GetDocuments().empty());
}

TEST(FEditorWorkspaceUITests, DocumentRootKeysRemainDistinctForSameNamedAssets)
{
	const std::string First = Durin::TabLayout::MakeDocumentRootKey(
		"MaterialEditor", "/Game/Environment/M_Stone"
	);
	const std::string Second = Durin::TabLayout::MakeDocumentRootKey(
		"MaterialEditor", "/Game/Props/M_Stone"
	);
	EXPECT_NE(First, Second);
	EXPECT_NE(
		Durin::TabLayout::MakeRootWindowName("M_Stone", First),
		Durin::TabLayout::MakeRootWindowName("M_Stone", Second)
	);
}

TEST(FEditorWorkspaceUITests, PreservesStableWorkspaceWindowAndDockIdentities)
{
	using namespace Durin;
	using namespace Durin;
	const FAssetEditorToolkitId ToolkitId("LevelEditor");

	EXPECT_EQ(TabLayout::MakeMainFrameDockSpaceName(2), "Durin.DockSpace.EditorHost.v2");
	EXPECT_EQ(
		TabLayout::MakeRootWindowName("Level Editor", "LevelEditor"),
		"Level Editor###Durin.Editor.Root.LevelEditor"
	);
	EXPECT_EQ(
		TabLayout::MakeDocumentRootKey("MaterialEditor", "/Game/Materials/M_Stone"),
		"MaterialEditor./Game/Materials/M_Stone"
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
		Durin::MakeThumbnailRenderer<FPickerThumbnailRenderer>());
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
