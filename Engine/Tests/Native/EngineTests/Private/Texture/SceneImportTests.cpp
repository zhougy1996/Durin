#include "TextureTestSupport.h"

#include "ImportRecord.h"
#include "Materials/MaterialInstance.h"
#include "SceneImport.h"
#include "StandardAssetImportProviders.h"
#include "StaticMesh/StaticMesh.h"

namespace
{
	auto MakeAssetPath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Result;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Result));
		return Result;
	}

	struct FSceneFixture
	{
		std::unique_ptr<Durin::PathUtilities::FScopedMountRegistryFixture> Mounts;
		Durin::FSourcePath Source;
		Durin::FAssetPath Primary;
	};

	auto InitializeSceneFixture(std::string_view Name) -> FSceneFixture
	{
		InitializeDObjectSystem();
		std::string Error;
		EXPECT_TRUE(Durin::RegisterStandardAssetImportProviders(Error)) << Error;
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "SceneImport" / std::string(Name);
		Durin::Testing::RemoveTestWorkDirectory(Root);
		for (const std::filesystem::path& Directory : {
			Root / "Engine/Content", Root / "Engine/SourceAssets",
			Root / "Project/Content", Root / "Project/SourceAssets"})
			std::filesystem::create_directories(Directory);
		auto Mounts = std::make_unique<Durin::PathUtilities::FScopedMountRegistryFixture>(
			std::vector<Durin::PathUtilities::FMountPoint>{
				{
					.VirtualRoot = "/Engine/",
					.Owner = Durin::PathUtilities::EMountOwner::Test,
					.OwnerRoot = Root / "Engine",
					.ContentRoot = Root / "Engine/Content",
					.SourceAssetsRoot = Root / "Engine/SourceAssets",
					.bSourceWritable = true},
				{
					.VirtualRoot = "/SceneImportTests/",
					.Owner = Durin::PathUtilities::EMountOwner::Test,
					.OwnerRoot = Root / "Project",
					.ContentRoot = Root / "Project/Content",
					.SourceAssetsRoot = Root / "Project/SourceAssets",
					.bSourceWritable = true,
					.Dependencies = {"/Engine/"}}});
		EXPECT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		EXPECT_NE(Durin::EnsureStandardImportedSurfaceMaterial(Error), nullptr)
			<< Error;
		const std::filesystem::path Destination =
			Root / "Project/SourceAssets" / "Scenes" / (std::string(Name) + ".gltf");
		std::filesystem::create_directories(Destination.parent_path());
		std::filesystem::copy_file(
			std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "StaticModelMaterials/RenderedOpaqueDataUri.gltf",
			Destination,
			std::filesystem::copy_options::overwrite_existing);
		return {
			.Mounts = std::move(Mounts),
			.Source = {.Path = std::format("/SceneImportTests/Scenes/{}.gltf", Name)},
			.Primary = MakeAssetPath(std::format(
				"/SceneImportTests/SceneImport/{}/Primary", Name))};
	}

	auto PlanAndExecute(const FSceneFixture& Fixture)
		-> Durin::FSceneImportExecutionResult
	{
		const Durin::FSceneImportPlanResult Planned = Durin::PlanSceneImport({
			.RootSource = Fixture.Source,
			.PrimaryOutput = Fixture.Primary,
			.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
		EXPECT_TRUE(Planned) << Planned.Message;
		if (!Planned) return {};
		EXPECT_EQ(
			Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs().size(), 3u);
		return Durin::ExecuteSceneImport(Planned.Plan);
	}
}

TEST(FSceneImportTests, PublishesHeterogeneousPeersUnderGenericRecord)
{
	const FSceneFixture Fixture = InitializeSceneFixture("Initial");
	const Durin::FSceneImportExecutionResult Executed = PlanAndExecute(Fixture);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_NE(Executed.Record, nullptr);
	ASSERT_NE(Executed.StaticMesh, nullptr);
	ASSERT_EQ(Executed.Materials.size(), 1u);
	ASSERT_EQ(Executed.Textures.size(), 1u);
	EXPECT_EQ(Executed.Record->GetProviderId(), Durin::SceneImportProviderId);
	EXPECT_EQ(Executed.Record->GetPrimaryOutput(), Fixture.Primary);
	EXPECT_EQ(Executed.Record->GetOutputs().size(), 3u);
}

TEST(FSceneImportTests, UsesProviderNeutralRecordCapabilitiesForReimport)
{
	const FSceneFixture Fixture = InitializeSceneFixture("RecordCapabilities");
	const Durin::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	Durin::FAssetPath RecordPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Initial.Record->GetPackage()->GetPackagePath(), RecordPath));
	const auto Inspection = Durin::AssetImport::InspectImportRecord(
		RecordPath, Durin::AssetImport::GetImportRecordIndex());
	ASSERT_TRUE(Inspection) << Inspection.Message;
	const auto Capabilities = Durin::AssetImport::QueryImportRecordCapabilities(
		Inspection, Durin::AssetImport::GetImportRecordHandlerRegistry());
	const auto* Reimport = Capabilities.Find(
		Durin::AssetImport::EImportRecordAction::Reimport);
	ASSERT_NE(Reimport, nullptr);
	ASSERT_TRUE(Reimport->bAvailable);
	const auto Executed = Durin::AssetImport::ExecuteImportRecordAction(
		*Inspection.Record,
		Durin::AssetImport::EImportRecordAction::Reimport,
		Durin::AssetImport::GetImportRecordHandlerRegistry());
	ASSERT_TRUE(Executed) << Executed.Message;
	EXPECT_EQ(Executed.Record, Initial.Record);
	EXPECT_EQ(Executed.Outputs.size(), 3u);
}

TEST(FSceneImportTests, RuntimeOutputsDoNotReflectSceneOwnershipState)
{
	InitializeDObjectSystem();
	EXPECT_EQ(Durin::DStaticMesh::StaticClass()->FindPropertyByName(
		Durin::FName("ImportManifest"), false), nullptr);
	EXPECT_EQ(Durin::DMaterialInstance::StaticClass()->FindPropertyByName(
		Durin::FName("ImportOwner"), false), nullptr);
	EXPECT_EQ(Durin::DTexture2D::StaticClass()->FindPropertyByName(
		Durin::FName("ImportOwner"), false), nullptr);
}

TEST(FSceneImportTests, IngestsExternalGltfBundleAndPlansFbxBeforePublication)
{
	const FSceneFixture Fixture = InitializeSceneFixture("ExternalBundle");
	Durin::FPreparedSceneSourceBundle Bundle;
	std::string Error;
	ASSERT_TRUE(Durin::PrepareSceneSourceBundle(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/MaterialContract.gltf",
		Fixture.Primary.ToString(),
		"/SceneImportTests/Ingested/MaterialContract.gltf",
		Bundle, Error)) << Error;
	ASSERT_EQ(Bundle.Sources.size(), 3u);
	Durin::CommitSceneSourceBundle(Bundle);
	const Durin::FSceneImportPlanResult Planned = Durin::PlanSceneImport({
		.RootSource = Bundle.RootSource,
		.PrimaryOutput = MakeAssetPath(
			"/SceneImportTests/SceneImport/ExternalBundle/Imported"),
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	EXPECT_TRUE(Planned) << Planned.Message;

	Durin::FPreparedSceneSourceBundle FbxBundle;
	ASSERT_TRUE(Durin::PrepareSceneSourceBundle(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/PhongMaterial.fbx",
		Fixture.Primary.ToString(),
		"/SceneImportTests/Ingested/PhongMaterial.fbx",
		FbxBundle, Error)) << Error;
	Durin::CommitSceneSourceBundle(FbxBundle);
	const Durin::FSceneImportPlanResult FbxPlanned = Durin::PlanSceneImport({
		.RootSource = FbxBundle.RootSource,
		.PrimaryOutput = MakeAssetPath(
			"/SceneImportTests/SceneImport/ExternalBundle/FbxImported"),
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	EXPECT_TRUE(FbxPlanned) << FbxPlanned.Message;
}

TEST(FSceneImportTests, ReimportsManagedPeersInPlaceAndKeepsRecordAuthoritative)
{
	const FSceneFixture Fixture = InitializeSceneFixture("Reimport");
	const Durin::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	const std::string PreviousRecordFingerprint = Initial.Record->GetFingerprint();
	Durin::DStaticMesh* Mesh = Initial.StaticMesh;
	Durin::DMaterialInstance* Material = Initial.Materials[0];
	Durin::DTexture2D* Texture = Initial.Textures[0];

	const Durin::FSceneImportPlanResult Planned =
		Durin::PlanSceneReimport(*Initial.Record);
	ASSERT_TRUE(Planned) << Planned.Message;
	const Durin::FSceneImportExecutionResult Reimported =
		Durin::ExecuteSceneImport(Planned.Plan);
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_EQ(Reimported.Record, Initial.Record);
	EXPECT_EQ(Reimported.StaticMesh, Mesh);
	ASSERT_EQ(Reimported.Materials.size(), 1u);
	ASSERT_EQ(Reimported.Textures.size(), 1u);
	EXPECT_EQ(Reimported.Materials[0], Material);
	EXPECT_EQ(Reimported.Textures[0], Texture);
	EXPECT_EQ(Reimported.Record->GetFingerprint(), PreviousRecordFingerprint);
}

TEST(FSceneImportTests, FailedRootLastReimportRestoresEveryPeerAndRecord)
{
	const FSceneFixture Fixture = InitializeSceneFixture("FailureRestore");
	const Durin::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	const std::string RecordFingerprint = Initial.Record->GetFingerprint();
	const std::string MeshFingerprint = Initial.StaticMesh->GetSourceImportData().SourceContentHash;
	const std::string TextureKey = Initial.Textures[0]->GetDerivedDataKey();

	const Durin::FSceneImportPlanResult Planned =
		Durin::PlanSceneReimport(*Initial.Record);
	ASSERT_TRUE(Planned) << Planned.Message;
	Durin::AssetImport::FMultiOutputExecutionOptions Options;
	Options.SaveOptions.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
	};
	const Durin::FSceneImportExecutionResult Failed =
		Durin::ExecuteSceneImport(Planned.Plan, Options);
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Initial.Record->GetFingerprint(), RecordFingerprint);
	EXPECT_EQ(Initial.StaticMesh->GetSourceImportData().SourceContentHash, MeshFingerprint);
	EXPECT_EQ(Initial.Textures[0]->GetDerivedDataKey(), TextureKey);
}

TEST(FSceneImportTests, RecordReloadDoesNotLoadOutputDependencyClosure)
{
	const FSceneFixture Fixture = InitializeSceneFixture("RecordReload");
	const Durin::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	Durin::FAssetPath RecordPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Initial.Record->GetPackage()->GetPackagePath(), RecordPath));
	std::vector<Durin::FAssetPath> Outputs;
	for (const Durin::AssetImport::FImportRecordOutput& Output
		: Initial.Record->GetOutputs()) Outputs.push_back(Output.AssetPath);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		MakeAssetPath(Initial.StaticMesh->GetPackage()->GetPackagePath())));
	for (Durin::DMaterialInstance* Material : Initial.Materials)
		ASSERT_TRUE(Durin::Asset::UnloadPackage(
			MakeAssetPath(Material->GetPackage()->GetPackagePath())));
	for (Durin::DTexture2D* Texture : Initial.Textures)
		ASSERT_TRUE(Durin::Asset::UnloadPackage(
			MakeAssetPath(Texture->GetPackage()->GetPackagePath())));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RecordPath));

	Durin::AssetImport::DImportRecord* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(RecordPath, Reloaded));
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetOutputs().size(), Outputs.size());
	for (const Durin::FAssetPath& Output : Outputs)
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(Output), nullptr);
}
