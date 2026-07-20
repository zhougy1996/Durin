#include "Editor/EditorWorkspace.h"

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
		auto ActivateDocument(const Durin::FEditorDocumentTab&) -> void override { ++ActivationCount; }
		auto DrawMainMenu() -> void override {}
		auto DrawWorkspace(bool) -> bool override { return false; }
		auto ResetLayout() -> void override {}

		int ActivationCount = 0;

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
}

TEST(FEditorWorkspaceManagerTests, CommitsWorkspaceAndAssetEditorsAsOneBatch)
{
	Durin::FEditorWorkspaceManager Manager;
	auto Workspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle Registration = Manager.RegisterBatch({
		.Workspaces = {Workspace},
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
	Durin::FEditorWorkspaceRegistrationHandle Existing = Manager.RegisterBatch({.Workspaces = {ExistingWorkspace}});
	ASSERT_TRUE(Existing);

	auto CandidateWorkspace = std::make_shared<FTestWorkspace>("MaterialEditor");
	Durin::FEditorWorkspaceRegistrationHandle DuplicateWorkspace = Manager.RegisterBatch({
		.Workspaces = {CandidateWorkspace, ExistingWorkspace},
	});
	EXPECT_FALSE(DuplicateWorkspace);
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), nullptr);

	Durin::FEditorWorkspaceRegistrationHandle DuplicateAsset = Manager.RegisterBatch({
		.Workspaces = {CandidateWorkspace},
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
		.Workspaces = {Workspace},
		.AssetEditors = {
			MakeAssetEditor("Material", "MaterialEditor"),
			MakeAssetEditor("Texture", "MissingEditor"),
		},
	});
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Manager.OpenAsset("/Game/M_Test", "Material"));

	Durin::FEditorWorkspaceRegistrationHandle Retry = Manager.RegisterBatch({
		.Workspaces = {Workspace},
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
		.Workspaces = {LevelWorkspace},
		.AssetEditors = {MakeAssetEditor("Level", "LevelEditor")},
	});
	Durin::FEditorWorkspaceRegistrationHandle MaterialRegistration = Manager.RegisterBatch({
		.Workspaces = {MaterialWorkspace},
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
		Registration = Manager.RegisterBatch({.Workspaces = {std::make_shared<FTestWorkspace>("MaterialEditor")}});
		ASSERT_TRUE(Registration);
	}
	EXPECT_FALSE(Registration.IsValid());
	Registration.Reset();
}
