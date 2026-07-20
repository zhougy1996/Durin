#include "Editor/EditorAssetPicker.h"
#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"

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
		auto OpenDocument(const Durin::FEditorDocumentTab&) -> bool override { return true; }
		auto ActivateDocument(const Durin::FEditorDocumentTab& Document) -> void override
		{
			++ActivationCount;
			LastActivatedResource = Document.ResourceId;
		}
		auto DrawMainMenu() -> void override {}
		auto DrawWorkspace(bool) -> bool override { return false; }
		auto ResetLayout() -> void override {}

		int ActivationCount = 0;
		std::string LastActivatedResource;

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
}
