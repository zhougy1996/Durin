#include "Asset/Asset.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Editor/AssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Editor/PropertyEditing.h"
#include "Editor/Transaction.h"
#include "Editor/Transactor.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Editor/WorkspaceUI.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "MonaImGui.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	class FAppliedPackageEdit final : public Durin::Editor::ITransactionCustomChange
	{
	public:
		explicit FAppliedPackageEdit(Durin::DPackage& InPackage)
			: Package{&InPackage}
		{
		}

		auto GetDescription() const -> std::string_view override { return "Edit Asset"; }
		auto GetAffectedPackages() const -> std::span<Durin::DPackage* const> override
		{
			return Package;
		}
		auto Undo() -> bool override { return true; }
		auto Redo() -> bool override { return true; }

	private:
		std::array<Durin::DPackage*, 1> Package{};
	};

	class FTestWorkspace final : public Durin::Editor::IWorkspace
	{
	public:
		explicit FTestWorkspace(std::string Type)
			: WorkspaceType(std::move(Type))
		{
		}

		auto GetWorkspaceType() const -> const Durin::Editor::FWorkspaceTypeId& override { return WorkspaceType; }
		auto OpenDocument(const Durin::Editor::FDocumentTab&) -> Durin::Editor::EDocumentOpenResult override
		{
			return OpenResult;
		}
		auto ActivateDocument(const Durin::Editor::FDocumentTab& Document) -> void override
		{
			++ActivationCount;
			LastActivatedResource = Document.ResourceId;
		}
		auto RequestDeactivate() -> bool override
		{
			++DeactivationRequestCount;
			return bAllowDeactivation;
		}
		auto RequestCloseDocument(const Durin::Editor::FDocumentTab&) -> Durin::Editor::EDocumentCloseResult override
		{
			++CloseRequestCount;
			return CloseResult;
		}
		auto SaveDocument(const Durin::Editor::FDocumentTab& Document) -> bool override
		{
			++SaveCount;
			LastSavedResource = Document.ResourceId;
			if (bAllowSave) CloseResult = Durin::Editor::EDocumentCloseResult::Closed;
			return bAllowSave;
		}
		auto DiscardDocument(const Durin::Editor::FDocumentTab& Document) -> bool override
		{
			++DiscardCount;
			LastDiscardedResource = Document.ResourceId;
			if (bAllowDiscard) CloseResult = Durin::Editor::EDocumentCloseResult::Closed;
			return bAllowDiscard;
		}
		auto DrawWorkspace(bool) -> bool override { return false; }
		auto ResetLayout() -> void override {}

		int ActivationCount = 0;
		int DeactivationRequestCount = 0;
		int CloseRequestCount = 0;
		int SaveCount = 0;
		int DiscardCount = 0;
		bool bAllowDeactivation = true;
		bool bAllowSave = true;
		bool bAllowDiscard = true;
		Durin::Editor::EDocumentOpenResult OpenResult = Durin::Editor::EDocumentOpenResult::Opened;
		Durin::Editor::EDocumentCloseResult CloseResult = Durin::Editor::EDocumentCloseResult::Closed;
		std::string LastActivatedResource;
		std::string LastSavedResource;
		std::string LastDiscardedResource;

	private:
		Durin::Editor::FWorkspaceTypeId WorkspaceType;
	};

	auto MakeAssetEditor(std::string AssetClassName, std::string WorkspaceType) -> Durin::Editor::FAssetEditorRegistration
	{
		return {
			.AssetClassName = std::move(AssetClassName),
			.WorkspaceType = Durin::Editor::FWorkspaceTypeId(std::move(WorkspaceType)),
		};
	}

	auto MakeWorkspaceRegistration(
		const std::shared_ptr<FTestWorkspace>& Workspace,
		std::string DisplayName = "Test Editor",
		std::string RootKey = {}
	) -> Durin::Editor::FWorkspaceRegistration
	{
		const std::string WorkspaceType(Workspace->GetWorkspaceType().GetValue());
		return {
			.Descriptor = {
				.WorkspaceType = Durin::Editor::FWorkspaceTypeId(WorkspaceType),
				.DisplayName = std::move(DisplayName),
				.RootKey = RootKey.empty() ? WorkspaceType : std::move(RootKey),
			},
			.Workspace = Workspace,
		};
	}
}

TEST(FEditorTransactorOwnershipTests, OwnsTransientBufferAndClearsOpenHistoryOnShutdown)
{
	Durin::Testing::InitializeDObjectSystemForTests();
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
		Durin::Editor::FScopedTransaction GlobalScope("Global editor transaction");
		ASSERT_TRUE(GlobalScope.IsActive());
		EXPECT_EQ(GlobalScope.Cancel().Code,
			Durin::Editor::ETransactorResultCode::Discarded);
	}

	const auto Scope = Buffer->Begin({"test", "Open during shutdown",
		Durin::Editor::FPersistentObjectRef(Editor)});
	ASSERT_TRUE(Scope);
	const Durin::Editor::FPropertyEditTarget Target =
		Durin::Editor::FPropertyEditTarget::ForMember(Editor, TransProperty);
	Durin::FPropertyValueSnapshotPayload Snapshot;
	std::string Error;
	ASSERT_TRUE(Durin::CapturePropertyValuePayload(
		TransProperty, Editor, 0, Snapshot, &Error)) << Error;
	Durin::Editor::FTransactionObjectRecord Record;
	ASSERT_TRUE(Durin::Editor::FTransactionObjectRecord::Capture(
		Target, Snapshot, Snapshot, Record, &Error)) << Error;
	ASSERT_TRUE(Buffer->Record(Scope.ScopeId, std::move(Record)));
	Editor->BeginDestroy();
	EXPECT_EQ(Editor->GetTransactor(), nullptr);
	EXPECT_EQ(Buffer->GetState(), Durin::Editor::ETransactorState::Destroying);
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
	EXPECT_EQ(Buffer->GetOwnedBytes(), 0u);
}

TEST(FEditableAssetDocumentModelTests, UndoToActivatedRevisionClearsDirtyState)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	auto* Editor = Durin::NewObject<Durin::DEditorEngine>(
		nullptr, "EditableAssetDocumentEditor");
	ASSERT_NE(Editor, nullptr);
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

	Durin::Editor::FEditableAssetDocumentModel Documents;
	const Durin::Editor::FDocumentTab Document{
		.Id = {1},
		.ResourceId = "/EditableAssetDocumentTests/UndoCheckpoint",
	};
	ASSERT_TRUE(Documents.Activate(Document, Asset));
	Durin::DTransactor& Transactions =
		*Editor->GetTransactor();
	const auto OpenRevision = Transactions.GetPackageRevisionState(*Package);
	ASSERT_TRUE(OpenRevision.has_value());
	EXPECT_EQ(OpenRevision->CurrentRevision, OpenRevision->SavedRevision);

	Package->MarkDirty();
	ASSERT_TRUE(Transactions.CommitApplied(
		std::make_unique<FAppliedPackageEdit>(*Package)));
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Documents.Undo());
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Documents.Redo());
	EXPECT_TRUE(Package->IsDirty());

	Durin::MarkObjectHierarchyAsGarbage(Package);
	Durin::MarkAsGarbage(Editor);
	Durin::CollectGarbage();
}

TEST(FEditorWorkspaceManagerTests, CommitsWorkspaceAndAssetEditorsAsOneBatch)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});

	ASSERT_TRUE(Registration);
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")), Workspace);
	EXPECT_TRUE(Manager.OpenAsset("/Game/Materials/M_Stone", "Material"));
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().Label, "M_Stone");
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

	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	auto Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace)},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});
	ASSERT_TRUE(Registration);
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/M_Visible", "Material"));

	std::vector<std::string> Events;
	ImGui::NewFrame();
	Durin::Editor::FWorkspaceDocumentHost DocumentHost;
	DocumentHost.DrawDocuments(
		Manager,
		Durin::Editor::FWorkspaceTypeId("MaterialEditor"),
		"MaterialEditor",
		[](const Durin::Editor::FDocumentTab&) { return true; },
		[&Events](const Durin::Editor::FDocumentTab&) { Events.emplace_back("draw"); },
		[&Events](const Durin::Editor::FDocumentTab&, bool bVisible) {
			Events.emplace_back(bVisible ? "visible" : "hidden");
		});
	ImGui::Render();
	ImGui::DestroyContext(Context);

	EXPECT_EQ(Events, (std::vector<std::string>{"visible", "draw"}));
}

TEST(FEditorWorkspaceManagerTests, UnregistrationRemovesLookupAndOwnerReleasesEscapedWorkspace)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("OwnedEditor");
	auto Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace)}});
	ASSERT_TRUE(Registration);
	auto Escaped = Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("OwnedEditor"));
	ASSERT_TRUE(Escaped);

	EXPECT_EQ(Escaped.get(), Workspace.get());
	const std::weak_ptr<FTestWorkspace> WeakWorkspace = Workspace;
	Workspace.reset();
	Registration.Reset();
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("OwnedEditor")), nullptr);
	EXPECT_FALSE(WeakWorkspace.expired());
	Escaped.reset();
	EXPECT_TRUE(WeakWorkspace.expired());
}

TEST(FEditorWorkspaceManagerTests, RejectsDuplicatesBeforeApplyingAnyBatchEntry)
{
	Durin::Editor::FWorkspaceManager Manager;
	EXPECT_FALSE(Manager.RegisterBatch({}));
	auto ExistingWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Existing = Manager.RegisterBatch({.Workspaces = {MakeWorkspaceRegistration(ExistingWorkspace)}});
	ASSERT_TRUE(Existing);

	auto CandidateWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle DuplicateWorkspace = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(CandidateWorkspace), MakeWorkspaceRegistration(ExistingWorkspace)},
	});
	EXPECT_FALSE(DuplicateWorkspace);
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);

	Durin::Editor::FWorkspaceRegistrationHandle DuplicateAsset = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(CandidateWorkspace)},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("Material", "MaterialEditor"),
		},
	});
	EXPECT_FALSE(DuplicateAsset);
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Manager.OpenAsset("/Game/M_Test", "Material"));
}

TEST(FEditorWorkspaceManagerTests, RollsBackInvalidBatchAndAllowsRetry)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Invalid = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace)},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("Texture", "MissingEditor"),
		},
	});
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Manager.OpenAsset("/Game/M_Test", "Material"));

	Durin::Editor::FWorkspaceRegistrationHandle Retry = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace)},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});
	EXPECT_TRUE(Retry);
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")), Workspace);
}

TEST(FEditorWorkspaceManagerTests, ScopedUnregistrationRemovesRoutesAndOwnedDocuments)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto LevelWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	auto MaterialWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle LevelRegistration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(LevelWorkspace, "Level Editor")},
		.AssetEditors = {MakeAssetEditor("Level", "LevelEditor")},
	});
	Durin::Editor::FWorkspaceRegistrationHandle MaterialRegistration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(MaterialWorkspace, "Material Editor")},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});
	ASSERT_TRUE(LevelRegistration);
	ASSERT_TRUE(MaterialRegistration);
	ASSERT_TRUE(Manager.OpenAsset("/Game/Maps/Main", "Level"));
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/M_Stone", "Material"));
	ASSERT_EQ(Manager.GetDocuments().size(), 2);

	MaterialRegistration.Reset();

	EXPECT_EQ(MaterialWorkspace->DeactivationRequestCount, 1);
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().WorkspaceType, Durin::Editor::FWorkspaceTypeId("LevelEditor"));
	ASSERT_NE(Manager.GetActiveDocument(), nullptr);
	EXPECT_EQ(Manager.GetActiveDocument()->WorkspaceType, Durin::Editor::FWorkspaceTypeId("LevelEditor"));
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Manager.OpenAsset("/Game/Materials/M_Other", "Material"));
	EXPECT_GT(LevelWorkspace->ActivationCount, 1);
}

TEST(FEditorWorkspaceManagerTests, RegistrationHandleMayOutliveManager)
{
	Durin::Editor::FWorkspaceRegistrationHandle Registration;
	{
		Durin::Editor::FWorkspaceManager Manager;
		auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
		Registration = Manager.RegisterBatch({.Workspaces = {MakeWorkspaceRegistration(Workspace)}});
		ASSERT_TRUE(Registration);
	}
	EXPECT_FALSE(Registration.IsValid());
	Registration.Reset();
}

TEST(FEditorWorkspaceManagerTests, RegistersOrderedDescriptorsAndOpensDefaults)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto LevelWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	auto MaterialWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistration Level = MakeWorkspaceRegistration(LevelWorkspace, "Level Editor");
	Level.Descriptor.bOpenByDefault = true;
	Level.Descriptor.SingletonDocumentKey = "LevelEditor";
	Level.Descriptor.SingletonDocumentLabel = "Level Editor";
	Durin::Editor::FWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {std::move(Level), MakeWorkspaceRegistration(MaterialWorkspace, "Material Editor")},
	});
	ASSERT_TRUE(Registration);

	const std::vector<Durin::Editor::FWorkspaceDescriptor> Descriptors = Manager.GetWorkspaceDescriptors();
	ASSERT_EQ(Descriptors.size(), 2);
	EXPECT_EQ(Descriptors[0].DisplayName, "Level Editor");
	EXPECT_EQ(Descriptors[1].DisplayName, "Material Editor");
	EXPECT_TRUE(Manager.OpenDefaultWorkspaces());
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().WorkspaceType, Durin::Editor::FWorkspaceTypeId("LevelEditor"));
}

TEST(FEditorWorkspaceManagerTests, RejectsInvalidOrCollidingDescriptorsBeforeMutation)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto LevelWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	auto MaterialWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistration InvalidDefault = MakeWorkspaceRegistration(LevelWorkspace);
	InvalidDefault.Descriptor.bOpenByDefault = true;
	EXPECT_FALSE(Manager.RegisterBatch({.Workspaces = {std::move(InvalidDefault)}}));

	Durin::Editor::FWorkspaceRegistration WrongType = MakeWorkspaceRegistration(LevelWorkspace);
	WrongType.Descriptor.WorkspaceType = Durin::Editor::FWorkspaceTypeId("WrongEditor");
	EXPECT_FALSE(Manager.RegisterBatch({.Workspaces = {std::move(WrongType)}}));

	EXPECT_FALSE(Manager.RegisterBatch({
		.Workspaces = {
			MakeWorkspaceRegistration(LevelWorkspace, "Level Editor", "SharedRoot"),
			MakeWorkspaceRegistration(MaterialWorkspace, "Material Editor", "SharedRoot"),
		},
	}));
	EXPECT_TRUE(Manager.GetWorkspaceDescriptors().empty());
}

TEST(FEditorWorkspaceManagerTests, OpensAndSwitchesMultiplePerResourceDocumentsInOneWorkspace)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("MaterialInstance", "MaterialEditor"),
		},
	});
	ASSERT_TRUE(Registration);
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/M_Stone", "Material"));
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/MI_Stone", "MaterialInstance"));
	ASSERT_EQ(Manager.GetDocuments().size(), 2);
	EXPECT_EQ(Manager.GetDocuments()[0].Label, "M_Stone");
	EXPECT_EQ(Manager.GetDocuments()[1].Label, "MI_Stone");
	EXPECT_EQ(Workspace->LastActivatedResource, "/Game/Materials/MI_Stone");

	EXPECT_TRUE(Manager.ActivateDocument(Manager.GetDocuments()[0].Id));
	EXPECT_EQ(Workspace->LastActivatedResource, "/Game/Materials/M_Stone");
}

TEST(FEditorWorkspaceManagerTests, RemapsOpenPerResourceDocumentsAfterAssetRelocation)
{
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});
	ASSERT_TRUE(Registration);
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/Old/M_Stone", "Material"));
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	const Durin::Editor::FDocumentId DocumentId = Manager.GetDocuments().front().Id;

	Manager.RemapResourceId(
		"/Game/Materials/Old/M_Stone", "/Game/Materials/New/M_Granite");
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	const Durin::Editor::FDocumentTab& Document = Manager.GetDocuments().front();
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
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("LevelEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Level Editor")},
	});
	ASSERT_TRUE(Registration);

	const Durin::Editor::FDocumentId DocumentId = Manager.OpenDocument({
		.WorkspaceType = Durin::Editor::FWorkspaceTypeId("LevelEditor"),
		.DocumentKey = "LevelEditor",
		.ResourceId = "/Game/Maps/First",
		.Label = "First",
	});
	ASSERT_TRUE(DocumentId.IsValid());
	ASSERT_EQ(Manager.GetDocuments().size(), 1);

	Workspace->OpenResult = Durin::Editor::EDocumentOpenResult::Deferred;
	const Durin::Editor::FDocumentId DeferredId = Manager.OpenDocument({
		.WorkspaceType = Durin::Editor::FWorkspaceTypeId("LevelEditor"),
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

	const Durin::Editor::FDocumentId SecondDeferredId = Manager.OpenDocument({
		.WorkspaceType = Durin::Editor::FWorkspaceTypeId("LevelEditor"),
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
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("MaterialInstance", "MaterialEditor"),
		},
	});
	ASSERT_TRUE(Registration);
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/M_Stone", "Material"));
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/MI_Stone", "MaterialInstance"));
	const Durin::Editor::FDocumentId First = Manager.GetDocuments()[0].Id;
	const Durin::Editor::FDocumentId Second = Manager.GetDocuments()[1].Id;
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
	Durin::Editor::FWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::Editor::FWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
	});
	ASSERT_TRUE(Registration);

	const Durin::Editor::FDocumentId First = Manager.OpenDocument({
		.WorkspaceType = Durin::Editor::FWorkspaceTypeId("MaterialEditor"),
		.DocumentKey = "First",
		.ResourceId = "/Game/Materials/M_First",
		.Label = "M_First",
	});
	const Durin::Editor::FDocumentId Second = Manager.OpenDocument({
		.WorkspaceType = Durin::Editor::FWorkspaceTypeId("MaterialEditor"),
		.DocumentKey = "Second",
		.ResourceId = "/Game/Materials/M_Second",
		.Label = "M_Second",
	});
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Second.IsValid());

	Workspace->CloseResult = Durin::Editor::EDocumentCloseResult::PendingConfirmation;
	EXPECT_EQ(Manager.RequestCloseDocument(First), Durin::Editor::EDocumentCloseResult::PendingConfirmation);
	ASSERT_NE(Manager.GetPendingCloseDocument(), nullptr);
	EXPECT_EQ(Manager.GetPendingCloseDocument()->Id, First);
	EXPECT_EQ(Manager.RequestCloseDocument(First), Durin::Editor::EDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::Editor::EDocumentCloseResult::Rejected);

	Workspace->bAllowSave = false;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::Editor::EDocumentCloseResponse::Save),
		Durin::Editor::EDocumentCloseResult::PendingConfirmation
	);
	EXPECT_EQ(Manager.GetDocuments().size(), 2);
	ASSERT_NE(Manager.GetPendingCloseDocument(), nullptr);

	Workspace->bAllowSave = true;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::Editor::EDocumentCloseResponse::Save),
		Durin::Editor::EDocumentCloseResult::Closed
	);
	EXPECT_EQ(Workspace->LastSavedResource, "/Game/Materials/M_First");
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().Id, Second);
	EXPECT_EQ(Manager.GetPendingCloseDocument(), nullptr);

	Workspace->CloseResult = Durin::Editor::EDocumentCloseResult::PendingConfirmation;
	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::Editor::EDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::Editor::EDocumentCloseResponse::Cancel),
		Durin::Editor::EDocumentCloseResult::Cancelled
	);
	EXPECT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetPendingCloseDocument(), nullptr);

	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::Editor::EDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::Editor::EDocumentCloseResponse::Discard),
		Durin::Editor::EDocumentCloseResult::Closed
	);
	EXPECT_EQ(Workspace->LastDiscardedResource, "/Game/Materials/M_Second");
	EXPECT_TRUE(Manager.GetDocuments().empty());
}

TEST(FEditorWorkspaceUITests, DocumentRootKeysRemainDistinctForSameNamedAssets)
{
	const std::string First = Durin::Editor::WorkspaceUI::MakeDocumentRootKey(
		"MaterialEditor", "/Game/Environment/M_Stone"
	);
	const std::string Second = Durin::Editor::WorkspaceUI::MakeDocumentRootKey(
		"MaterialEditor", "/Game/Props/M_Stone"
	);
	EXPECT_NE(First, Second);
	EXPECT_NE(
		Durin::Editor::WorkspaceUI::MakeRootWindowName("M_Stone", First),
		Durin::Editor::WorkspaceUI::MakeRootWindowName("M_Stone", Second)
	);
}

TEST(FEditorWorkspaceUITests, PreservesStableWorkspaceWindowAndDockIdentities)
{
	using namespace Durin::Editor;
	const FWorkspaceTypeId WorkspaceType("LevelEditor");

	EXPECT_EQ(WorkspaceUI::MakeHostDockSpaceName(2), "Durin.DockSpace.EditorHost.v2");
	EXPECT_EQ(
		WorkspaceUI::MakeRootWindowName("Level Editor", "LevelEditor"),
		"Level Editor###Durin.Editor.Root.LevelEditor"
	);
	EXPECT_EQ(
		WorkspaceUI::MakeDocumentRootKey("MaterialEditor", "/Game/Materials/M_Stone"),
		"MaterialEditor./Game/Materials/M_Stone"
	);
	EXPECT_EQ(WorkspaceUI::MakeDockClassName(WorkspaceType), "Durin.DockClass.LevelEditor");
	EXPECT_EQ(WorkspaceUI::MakeDockSpaceName(WorkspaceType, 4), "Durin.DockSpace.LevelEditor.v4");
	EXPECT_EQ(
		WorkspaceUI::MakePanelWindowName("Details", WorkspaceType, "Details"),
		"Details###Durin.LevelEditor.Panel.Details"
	);
	EXPECT_EQ(WorkspaceUI::MakeRootDockClassId(), ImHashStr("Durin.DockClass.EditorRoot"));
	EXPECT_EQ(WorkspaceUI::MakeHostDockSpaceId(2), ImHashStr("Durin.DockSpace.EditorHost.v2"));
	EXPECT_EQ(WorkspaceUI::MakeDockClassId(WorkspaceType), ImHashStr("Durin.DockClass.LevelEditor"));
	EXPECT_EQ(WorkspaceUI::MakeDockSpaceId(WorkspaceType, 4), ImHashStr("Durin.DockSpace.LevelEditor.v4"));
}

TEST(FAssetPickerTests, AppliesExactAndDerivedClassPolicies)
{
	EXPECT_TRUE(Durin::Editor::AssetPicker::MatchesClass(
		Durin::DMaterialInstance::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::Editor::EAssetClassPolicy::Derived
	));
	EXPECT_FALSE(Durin::Editor::AssetPicker::MatchesClass(
		Durin::DMaterialInstance::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::Editor::EAssetClassPolicy::Exact
	));
	EXPECT_TRUE(Durin::Editor::AssetPicker::MatchesClass(
		Durin::DMaterialInterface::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::Editor::EAssetClassPolicy::Exact
	));
	EXPECT_TRUE(Durin::Editor::AssetPicker::MatchesClass(
		Durin::DTexture2D::StaticClass(),
		Durin::DTexture::StaticClass(),
		Durin::Editor::EAssetClassPolicy::Derived
	));
	EXPECT_TRUE(Durin::Editor::AssetPicker::MatchesClass(
		Durin::DTextureCube::StaticClass(),
		Durin::DTexture::StaticClass(),
		Durin::Editor::EAssetClassPolicy::Derived
	));
	EXPECT_FALSE(Durin::Editor::AssetPicker::MatchesClass(
		Durin::DTexture::StaticClass(),
		Durin::DTexture2D::StaticClass(),
		Durin::Editor::EAssetClassPolicy::Derived
	));
}

TEST(FAssetPickerTests, UsesSoftPathForUnloadedCurrentSelection)
{
	EXPECT_EQ(
		Durin::Editor::AssetPicker::GetAssetPathOrNone(nullptr, "/Game/Levels/Default", "None"),
		"/Game/Levels/Default"
	);
	EXPECT_EQ(Durin::Editor::AssetPicker::GetAssetPathOrNone(nullptr, {}, "None"), "None");
}

TEST(FAssetPickerTests, CanPresentExactSelectionsAsPackagePaths)
{
	EXPECT_EQ(
		Durin::Editor::AssetPicker::GetAssetPathDisplayName(
			"/Game/Levels/GrayboxStage15.GrayboxStage15",
			Durin::Editor::EAssetPathDisplayMode::PackagePath),
		"/Game/Levels/GrayboxStage15");
	EXPECT_EQ(
		Durin::Editor::AssetPicker::GetAssetPathDisplayName(
			"/Game/Levels/GrayboxStage15.GrayboxStage15",
			Durin::Editor::EAssetPathDisplayMode::ExactAssetPath),
		"/Game/Levels/GrayboxStage15.GrayboxStage15");
}

TEST(FAssetPickerTests, FiltersCandidatesByPathPrefix)
{
	EXPECT_TRUE(Durin::Editor::AssetPicker::MatchesPathPrefix("/Engine/Materials/Default", {}));
	EXPECT_TRUE(Durin::Editor::AssetPicker::MatchesPathPrefix("/Game/Levels/Default", "/Game/"));
	EXPECT_TRUE(Durin::Editor::AssetPicker::MatchesPathPrefix("/Game/Levels/Default", "/Game/Levels/"));
	EXPECT_FALSE(Durin::Editor::AssetPicker::MatchesPathPrefix("/Engine/Levels/Default", "/Game/"));
	EXPECT_FALSE(Durin::Editor::AssetPicker::MatchesPathPrefix("/Gameplay/Levels/Default", "/Game/"));
}
