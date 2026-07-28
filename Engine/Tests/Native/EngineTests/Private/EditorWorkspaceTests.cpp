#include "Editor/EditorAssetPicker.h"
#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

#include <gtest/gtest.h>

namespace
{
	class FTestWorkspace final : public Durin::IEditorWorkspace
	{
	public:
		explicit FTestWorkspace(std::string Type)
			: WorkspaceType(std::move(Type))
		{
		}

		auto GetWorkspaceType() const -> const Durin::FEditorWorkspaceTypeId& override { return WorkspaceType; }
		auto OpenDocument(const Durin::FEditorDocumentTab&) -> Durin::EEditorDocumentOpenResult override
		{
			return OpenResult;
		}
		auto ActivateDocument(const Durin::FEditorDocumentTab& Document) -> void override
		{
			++ActivationCount;
			LastActivatedResource = Document.ResourceId;
		}
		auto RequestDeactivate() -> bool override
		{
			++DeactivationRequestCount;
			return bAllowDeactivation;
		}
		auto RequestCloseDocument(const Durin::FEditorDocumentTab&) -> Durin::EEditorDocumentCloseResult override
		{
			++CloseRequestCount;
			return CloseResult;
		}
		auto SaveDocument(const Durin::FEditorDocumentTab& Document) -> bool override
		{
			++SaveCount;
			LastSavedResource = Document.ResourceId;
			if (bAllowSave) CloseResult = Durin::EEditorDocumentCloseResult::Closed;
			return bAllowSave;
		}
		auto DiscardDocument(const Durin::FEditorDocumentTab& Document) -> bool override
		{
			++DiscardCount;
			LastDiscardedResource = Document.ResourceId;
			if (bAllowDiscard) CloseResult = Durin::EEditorDocumentCloseResult::Closed;
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
		Durin::EEditorDocumentOpenResult OpenResult = Durin::EEditorDocumentOpenResult::Opened;
		Durin::EEditorDocumentCloseResult CloseResult = Durin::EEditorDocumentCloseResult::Closed;
		std::string LastActivatedResource;
		std::string LastSavedResource;
		std::string LastDiscardedResource;

	private:
		Durin::FEditorWorkspaceTypeId WorkspaceType;
	};

	auto MakeAssetEditor(std::string AssetClassName, std::string WorkspaceType) -> Durin::FEditorAssetEditorRegistration
	{
		return {
			.AssetClassName = std::move(AssetClassName),
			.WorkspaceType = Durin::FEditorWorkspaceTypeId(std::move(WorkspaceType)),
		};
	}

	auto MakeWorkspaceRegistration(
		const std::shared_ptr<FTestWorkspace>& Workspace,
		std::string DisplayName = "Test Editor",
		std::string RootKey = {}
	) -> Durin::FEditorWorkspaceRegistration
	{
		const std::string WorkspaceType(Workspace->GetWorkspaceType().GetValue());
		return {
			.Descriptor = {
				.WorkspaceType = Durin::FEditorWorkspaceTypeId(WorkspaceType),
				.DisplayName = std::move(DisplayName),
				.RootKey = RootKey.empty() ? WorkspaceType : std::move(RootKey),
			},
			.Workspace = Workspace,
		};
	}
}

TEST(FEditorWorkspaceManagerTests, CommitsWorkspaceAndAssetEditorsAsOneBatch)
{
	Durin::FEditorWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});

	ASSERT_TRUE(Registration);
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), Workspace);
	EXPECT_TRUE(Manager.OpenAsset("/Game/Materials/M_Stone", "Material"));
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().Label, "M_Stone");
}

TEST(FEditorWorkspaceManagerTests, RejectsDuplicatesBeforeApplyingAnyBatchEntry)
{
	Durin::FEditorWorkspaceManager Manager;
	EXPECT_FALSE(Manager.RegisterBatch({}));
	auto ExistingWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	Durin::FEditorWorkspaceRegistrationHandle Existing = Manager.RegisterBatch({.Workspaces = {MakeWorkspaceRegistration(ExistingWorkspace)}});
	ASSERT_TRUE(Existing);

	auto CandidateWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle DuplicateWorkspace = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(CandidateWorkspace), MakeWorkspaceRegistration(ExistingWorkspace)},
	});
	EXPECT_FALSE(DuplicateWorkspace);
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), nullptr);

	Durin::FEditorWorkspaceRegistrationHandle DuplicateAsset = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(CandidateWorkspace)},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("Material", "MaterialEditor"),
		},
	});
	EXPECT_FALSE(DuplicateAsset);
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Manager.OpenAsset("/Game/M_Test", "Material"));
}

TEST(FEditorWorkspaceManagerTests, RollsBackInvalidBatchAndAllowsRetry)
{
	Durin::FEditorWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle Invalid = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace)},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("Texture", "MissingEditor"),
		},
	});
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Manager.OpenAsset("/Game/M_Test", "Material"));

	Durin::FEditorWorkspaceRegistrationHandle Retry = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace)},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});
	EXPECT_TRUE(Retry);
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), Workspace);
}

TEST(FEditorWorkspaceManagerTests, ScopedUnregistrationRemovesRoutesAndOwnedDocuments)
{
	Durin::FEditorWorkspaceManager Manager;
	auto LevelWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	auto MaterialWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle LevelRegistration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(LevelWorkspace, "Level Editor")},
		.AssetEditors = {MakeAssetEditor("Level", "LevelEditor")},
	});
	Durin::FEditorWorkspaceRegistrationHandle MaterialRegistration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(MaterialWorkspace, "Material Editor")},
		.AssetEditors = {MakeAssetEditor("Material", "MaterialEditor")},
	});
	ASSERT_TRUE(LevelRegistration);
	ASSERT_TRUE(MaterialRegistration);
	ASSERT_TRUE(Manager.OpenAsset("/Game/Maps/Main", "Level"));
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/M_Stone", "Material"));
	ASSERT_EQ(Manager.GetDocuments().size(), 2);

	MaterialRegistration.Reset();

	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().WorkspaceType, Durin::FEditorWorkspaceTypeId("LevelEditor"));
	ASSERT_NE(Manager.GetActiveDocument(), nullptr);
	EXPECT_EQ(Manager.GetActiveDocument()->WorkspaceType, Durin::FEditorWorkspaceTypeId("LevelEditor"));
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Manager.OpenAsset("/Game/Materials/M_Other", "Material"));
	EXPECT_GT(LevelWorkspace->ActivationCount, 1);
}

TEST(FEditorWorkspaceManagerTests, RegistrationHandleMayOutliveManager)
{
	Durin::FEditorWorkspaceRegistrationHandle Registration;
	{
		Durin::FEditorWorkspaceManager Manager;
		auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
		Registration = Manager.RegisterBatch({.Workspaces = {MakeWorkspaceRegistration(Workspace)}});
		ASSERT_TRUE(Registration);
	}
	EXPECT_FALSE(Registration.IsValid());
	Registration.Reset();
}

TEST(FEditorWorkspaceManagerTests, RegistersOrderedDescriptorsAndOpensDefaults)
{
	Durin::FEditorWorkspaceManager Manager;
	auto LevelWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	auto MaterialWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistration Level = MakeWorkspaceRegistration(LevelWorkspace, "Level Editor");
	Level.Descriptor.bOpenByDefault = true;
	Level.Descriptor.SingletonDocumentKey = "LevelEditor";
	Level.Descriptor.SingletonDocumentLabel = "Level Editor";
	Durin::FEditorWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {std::move(Level), MakeWorkspaceRegistration(MaterialWorkspace, "Material Editor")},
	});
	ASSERT_TRUE(Registration);

	const std::vector<Durin::FEditorWorkspaceDescriptor> Descriptors = Manager.GetWorkspaceDescriptors();
	ASSERT_EQ(Descriptors.size(), 2);
	EXPECT_EQ(Descriptors[0].DisplayName, "Level Editor");
	EXPECT_EQ(Descriptors[1].DisplayName, "Material Editor");
	EXPECT_TRUE(Manager.OpenDefaultWorkspaces());
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().WorkspaceType, Durin::FEditorWorkspaceTypeId("LevelEditor"));
}

TEST(FEditorWorkspaceManagerTests, RejectsInvalidOrCollidingDescriptorsBeforeMutation)
{
	Durin::FEditorWorkspaceManager Manager;
	auto LevelWorkspace = std::make_shared<FTestWorkspace>("LevelEditor");
	auto MaterialWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistration InvalidDefault = MakeWorkspaceRegistration(LevelWorkspace);
	InvalidDefault.Descriptor.bOpenByDefault = true;
	EXPECT_FALSE(Manager.RegisterBatch({.Workspaces = {std::move(InvalidDefault)}}));

	Durin::FEditorWorkspaceRegistration WrongType = MakeWorkspaceRegistration(LevelWorkspace);
	WrongType.Descriptor.WorkspaceType = Durin::FEditorWorkspaceTypeId("WrongEditor");
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
	Durin::FEditorWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
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

TEST(FEditorWorkspaceManagerTests, CommitsDeferredSingletonReplacementOnlyAfterCompletion)
{
	Durin::FEditorWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("LevelEditor");
	Durin::FEditorWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Level Editor")},
	});
	ASSERT_TRUE(Registration);

	const Durin::FEditorDocumentId DocumentId = Manager.OpenDocument({
		.WorkspaceType = Durin::FEditorWorkspaceTypeId("LevelEditor"),
		.DocumentKey = "LevelEditor",
		.ResourceId = "/Game/Maps/First",
		.Label = "First",
	});
	ASSERT_TRUE(DocumentId.IsValid());
	ASSERT_EQ(Manager.GetDocuments().size(), 1);

	Workspace->OpenResult = Durin::EEditorDocumentOpenResult::Deferred;
	const Durin::FEditorDocumentId DeferredId = Manager.OpenDocument({
		.WorkspaceType = Durin::FEditorWorkspaceTypeId("LevelEditor"),
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

	const Durin::FEditorDocumentId SecondDeferredId = Manager.OpenDocument({
		.WorkspaceType = Durin::FEditorWorkspaceTypeId("LevelEditor"),
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
	Durin::FEditorWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("MaterialInstance", "MaterialEditor"),
		},
	});
	ASSERT_TRUE(Registration);
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/M_Stone", "Material"));
	ASSERT_TRUE(Manager.OpenAsset("/Game/Materials/MI_Stone", "MaterialInstance"));
	const Durin::FEditorDocumentId First = Manager.GetDocuments()[0].Id;
	const Durin::FEditorDocumentId Second = Manager.GetDocuments()[1].Id;
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
	Durin::FEditorWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {MakeWorkspaceRegistration(Workspace, "Material Editor")},
	});
	ASSERT_TRUE(Registration);

	const Durin::FEditorDocumentId First = Manager.OpenDocument({
		.WorkspaceType = Durin::FEditorWorkspaceTypeId("MaterialEditor"),
		.DocumentKey = "First",
		.ResourceId = "/Game/Materials/M_First",
		.Label = "M_First",
	});
	const Durin::FEditorDocumentId Second = Manager.OpenDocument({
		.WorkspaceType = Durin::FEditorWorkspaceTypeId("MaterialEditor"),
		.DocumentKey = "Second",
		.ResourceId = "/Game/Materials/M_Second",
		.Label = "M_Second",
	});
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Second.IsValid());

	Workspace->CloseResult = Durin::EEditorDocumentCloseResult::PendingConfirmation;
	EXPECT_EQ(Manager.RequestCloseDocument(First), Durin::EEditorDocumentCloseResult::PendingConfirmation);
	ASSERT_NE(Manager.GetPendingCloseDocument(), nullptr);
	EXPECT_EQ(Manager.GetPendingCloseDocument()->Id, First);
	EXPECT_EQ(Manager.RequestCloseDocument(First), Durin::EEditorDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::EEditorDocumentCloseResult::Rejected);

	Workspace->bAllowSave = false;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EEditorDocumentCloseResponse::Save),
		Durin::EEditorDocumentCloseResult::PendingConfirmation
	);
	EXPECT_EQ(Manager.GetDocuments().size(), 2);
	ASSERT_NE(Manager.GetPendingCloseDocument(), nullptr);

	Workspace->bAllowSave = true;
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EEditorDocumentCloseResponse::Save),
		Durin::EEditorDocumentCloseResult::Closed
	);
	EXPECT_EQ(Workspace->LastSavedResource, "/Game/Materials/M_First");
	ASSERT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetDocuments().front().Id, Second);
	EXPECT_EQ(Manager.GetPendingCloseDocument(), nullptr);

	Workspace->CloseResult = Durin::EEditorDocumentCloseResult::PendingConfirmation;
	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::EEditorDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EEditorDocumentCloseResponse::Cancel),
		Durin::EEditorDocumentCloseResult::Cancelled
	);
	EXPECT_EQ(Manager.GetDocuments().size(), 1);
	EXPECT_EQ(Manager.GetPendingCloseDocument(), nullptr);

	EXPECT_EQ(Manager.RequestCloseDocument(Second), Durin::EEditorDocumentCloseResult::PendingConfirmation);
	EXPECT_EQ(
		Manager.ResolvePendingDocumentClose(Durin::EEditorDocumentCloseResponse::Discard),
		Durin::EEditorDocumentCloseResult::Closed
	);
	EXPECT_EQ(Workspace->LastDiscardedResource, "/Game/Materials/M_Second");
	EXPECT_TRUE(Manager.GetDocuments().empty());
}

TEST(FEditorWorkspaceUITests, DocumentRootKeysRemainDistinctForSameNamedAssets)
{
	const std::string First = Durin::EditorWorkspaceUI::MakeEditorDocumentRootKey(
		"MaterialEditor", "/Game/Environment/M_Stone"
	);
	const std::string Second = Durin::EditorWorkspaceUI::MakeEditorDocumentRootKey(
		"MaterialEditor", "/Game/Props/M_Stone"
	);
	EXPECT_NE(First, Second);
	EXPECT_NE(
		Durin::EditorWorkspaceUI::MakeEditorRootWindowName("M_Stone", First),
		Durin::EditorWorkspaceUI::MakeEditorRootWindowName("M_Stone", Second)
	);
}

TEST(FEditorAssetPickerTests, AppliesExactAndDerivedClassPolicies)
{
	EXPECT_TRUE(Durin::EditorAssetPicker::MatchesClass(
		Durin::DMaterialInstance::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::EEditorAssetClassPolicy::Derived
	));
	EXPECT_FALSE(Durin::EditorAssetPicker::MatchesClass(
		Durin::DMaterialInstance::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::EEditorAssetClassPolicy::Exact
	));
	EXPECT_TRUE(Durin::EditorAssetPicker::MatchesClass(
		Durin::DMaterialInterface::StaticClass(),
		Durin::DMaterialInterface::StaticClass(),
		Durin::EEditorAssetClassPolicy::Exact
	));
	EXPECT_TRUE(Durin::EditorAssetPicker::MatchesClass(
		Durin::DTexture2D::StaticClass(),
		Durin::DTexture::StaticClass(),
		Durin::EEditorAssetClassPolicy::Derived
	));
	EXPECT_TRUE(Durin::EditorAssetPicker::MatchesClass(
		Durin::DTextureCube::StaticClass(),
		Durin::DTexture::StaticClass(),
		Durin::EEditorAssetClassPolicy::Derived
	));
	EXPECT_FALSE(Durin::EditorAssetPicker::MatchesClass(
		Durin::DTexture::StaticClass(),
		Durin::DTexture2D::StaticClass(),
		Durin::EEditorAssetClassPolicy::Derived
	));
}

TEST(FEditorAssetPickerTests, UsesSoftPathForUnloadedCurrentSelection)
{
	EXPECT_EQ(
		Durin::EditorAssetPicker::GetAssetPathOrNone(nullptr, "/Game/Levels/Default", "None"),
		"/Game/Levels/Default"
	);
	EXPECT_EQ(Durin::EditorAssetPicker::GetAssetPathOrNone(nullptr, {}, "None"), "None");
}

TEST(FEditorAssetPickerTests, FiltersCandidatesByPathPrefix)
{
	EXPECT_TRUE(Durin::EditorAssetPicker::MatchesPathPrefix("/Engine/Materials/Default", {}));
	EXPECT_TRUE(Durin::EditorAssetPicker::MatchesPathPrefix("/Game/Levels/Default", "/Game/"));
	EXPECT_TRUE(Durin::EditorAssetPicker::MatchesPathPrefix("/Game/Levels/Default", "/Game/Levels/"));
	EXPECT_FALSE(Durin::EditorAssetPicker::MatchesPathPrefix("/Engine/Levels/Default", "/Game/"));
	EXPECT_FALSE(Durin::EditorAssetPicker::MatchesPathPrefix("/Gameplay/Levels/Default", "/Game/"));
}
