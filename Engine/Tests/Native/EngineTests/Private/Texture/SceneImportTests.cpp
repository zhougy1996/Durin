#include "TextureTestSupport.h"

#include "AssetSystem.h"
#include "ImportRecord.h"
#include "Materials/MaterialInstance.h"
#include "SceneImport.h"
#include "StandardAssetImportProviders.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/Task.h"

namespace
{
	struct FAsyncImportSchedulerGuard
	{
		~FAsyncImportSchedulerGuard()
		{
			Durin::AssetImport::CancelAndDrainAllAsyncImports();
			Durin::ShutdownTaskScheduler(false);
		}
	};

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
		Durin::FAssetPath DestinationDirectory;
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
			.DestinationDirectory = MakeAssetPath(std::format(
				"/SceneImportTests/SceneImport/{}", Name))};
	}

	auto PlanAndExecute(const FSceneFixture& Fixture)
		-> Durin::FSceneImportExecutionResult
	{
		const Durin::FSceneImportPlanResult Planned = Durin::PlanSceneImport({
			.RootSource = Fixture.Source,
			.DestinationDirectory = Fixture.DestinationDirectory,
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
	ASSERT_EQ(Executed.Meshes.size(), 1u);
	ASSERT_EQ(Executed.Materials.size(), 1u);
	ASSERT_EQ(Executed.Textures.size(), 1u);
	EXPECT_EQ(Executed.Record->GetProviderId(), Durin::SceneImportProviderId);
	EXPECT_FALSE(Executed.Record->GetPrimaryOutput().IsValid());
	EXPECT_EQ(Executed.Record->GetOutputs().size(), 3u);
	EXPECT_TRUE(Executed.Record->IsCookExcluded());
	EXPECT_EQ(std::filesystem::path(Executed.Materials[0]->GetPackage()->GetPackagePath())
		.parent_path().generic_string(),
		"/SceneImportTests/SceneImport/Initial/Materials");
	EXPECT_EQ(std::filesystem::path(Executed.Textures[0]->GetPackage()->GetPackagePath())
		.parent_path().generic_string(),
		"/SceneImportTests/SceneImport/Initial/Textures");
}

TEST(FSceneImportTests, PlansPeerOutputsInsideTypedSceneDirectories)
{
	FSceneFixture Fixture = InitializeSceneFixture("Robot");
	Fixture.DestinationDirectory = MakeAssetPath(
		"/SceneImportTests/Scenes/Robot");
	const Durin::FSceneImportPlanResult Planned = Durin::PlanSceneImport({
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(Planned) << Planned.Message;

	const Durin::AssetImport::FImportPlan& Generic =
		Planned.Plan.GetMultiOutputPlan().GetGenericPlan();
	ASSERT_EQ(Generic.GetOutputs().size(), 3u);
	for (const Durin::AssetImport::FImportOutputPreview& Output : Generic.GetOutputs())
	{
		const std::string Parent = std::filesystem::path(
			Output.AssetPath.ToString()).parent_path().generic_string();
		if (Output.Role == "StaticMesh")
		{
			EXPECT_EQ(Parent, "/SceneImportTests/Scenes/Robot/Meshes");
			EXPECT_EQ(Output.AssetPath.ToString(),
				"/SceneImportTests/Scenes/Robot/Meshes/Robot");
		}
		else if (Output.Role == "MaterialInstance")
			EXPECT_EQ(Parent, "/SceneImportTests/Scenes/Robot/Materials");
		else if (Output.Role == "Texture2D.BaseColor")
			EXPECT_EQ(Parent, "/SceneImportTests/Scenes/Robot/Textures");
		else
			FAIL() << "Unexpected Scene output role: " << Output.Role;
	}
	EXPECT_FALSE(Planned.Plan.GetMultiOutputPlan().GetPrimaryOutput().IsValid());
	EXPECT_EQ(Planned.Plan.GetMultiOutputPlan().GetRecordPath().ToString(),
		"/SceneImportTests/Scenes/Robot/Robot_Import");
}

TEST(FSceneImportTests, AsyncPreparationMatchesSynchronousScenePlan)
{
	Durin::ShutdownTaskScheduler(false);
	FAsyncImportSchedulerGuard SchedulerGuard;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const FSceneFixture Fixture = InitializeSceneFixture("AsyncEquivalence");
	Durin::FSceneImportRequest Request{
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()};
	Durin::FSceneImportPlanResult Synchronous = Durin::PlanSceneImport(Request);
	ASSERT_TRUE(Synchronous) << Synchronous.Message;
	Durin::FSceneImportExecutionResult Initial =
		Durin::ExecuteSceneImport(Synchronous.Plan);
	ASSERT_TRUE(Initial) << Initial.Message;
	ASSERT_NE(Initial.Record, nullptr);
	ASSERT_EQ(Initial.Meshes.size(), 1u);
	ASSERT_EQ(Initial.Textures.size(), 1u);
	std::vector<std::pair<Durin::DPackage*, std::vector<Durin::uint8>>> BeforeBytes;
	auto CaptureBytes = [&](Durin::DPackage* Package) {
		ASSERT_NE(Package, nullptr);
		std::vector<Durin::uint8> Bytes;
		const Durin::Asset::FAssetResult Serialized =
			Durin::Asset::SerializeAssetPackageBytes(Package, Bytes);
		ASSERT_TRUE(Serialized) << Serialized.Message;
		BeforeBytes.emplace_back(Package, std::move(Bytes));
	};
	CaptureBytes(Initial.Record->GetPackage());
	CaptureBytes(Initial.Meshes[0]->GetPackage());
	for (Durin::DMaterialInstance* Material : Initial.Materials)
		CaptureBytes(Material->GetPackage());
	for (Durin::DTexture2D* Texture : Initial.Textures)
		CaptureBytes(Texture->GetPackage());
	const std::string TextureDerivedDataKey = Initial.Textures[0]->GetDerivedDataKey();
	Request.ExistingRecord = Initial.Record;
	Durin::FSceneImportAsyncPlanHandle Handle = Durin::BeginSceneImportPlan(
		Request, "Tests.SceneImport.AsyncEquivalence");
	ASSERT_TRUE(Handle);
	Durin::FSceneImportPlanResult Asynchronous;
	Durin::AssetImport::EAsyncImportPlanStatus Status =
		Durin::AssetImport::EAsyncImportPlanStatus::Pending;
	for (Durin::uint32 Attempt = 0; Attempt < 10'000
		&& Status == Durin::AssetImport::EAsyncImportPlanStatus::Pending; ++Attempt)
	{
		Status = Durin::PollSceneImportPlan(Handle, Asynchronous);
		if (Status == Durin::AssetImport::EAsyncImportPlanStatus::Pending)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(Status, Durin::AssetImport::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Asynchronous) << Asynchronous.Message;
	const auto& SyncGeneric =
		Synchronous.Plan.GetMultiOutputPlan().GetGenericPlan();
	const auto& AsyncGeneric =
		Asynchronous.Plan.GetMultiOutputPlan().GetGenericPlan();
	EXPECT_EQ(SyncGeneric.GetFingerprint(), AsyncGeneric.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(
		SyncGeneric.GetOutputs(), AsyncGeneric.GetOutputs()));
	EXPECT_EQ(Synchronous.Diagnostics, Asynchronous.Diagnostics);
	const Durin::FSceneImportExecutionResult Canceled = Durin::ExecuteSceneImport(
		Asynchronous.Plan, {
			.IsCancellationRequested = [] { return true; }});
	ASSERT_FALSE(Canceled);
	ASSERT_FALSE(Canceled.Diagnostics.empty());
	EXPECT_EQ(Canceled.Diagnostics.back().Category,
		Durin::AssetImport::EImportDiagnosticCategory::Canceled);
	Durin::FSceneImportExecutionResult Reimported =
		Durin::ExecuteSceneImport(Asynchronous.Plan);
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_EQ(Reimported.Record, Initial.Record);
	ASSERT_EQ(Reimported.Textures.size(), 1u);
	EXPECT_EQ(Reimported.Textures[0]->GetDerivedDataKey(), TextureDerivedDataKey);
	for (const auto& [Package, ExpectedBytes] : BeforeBytes)
	{
		std::vector<Durin::uint8> ActualBytes;
		const Durin::Asset::FAssetResult Serialized =
			Durin::Asset::SerializeAssetPackageBytes(Package, ActualBytes);
		ASSERT_TRUE(Serialized) << Serialized.Message;
		EXPECT_EQ(ActualBytes, ExpectedBytes);
	}
	Synchronous = {};
	Asynchronous = {};
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
		Fixture.DestinationDirectory.ToString(),
		"/SceneImportTests/Ingested/MaterialContract.gltf",
		Bundle, Error)) << Error;
	ASSERT_EQ(Bundle.Sources.size(), 3u);
	Durin::CommitSceneSourceBundle(Bundle);
	const Durin::FSceneImportPlanResult Planned = Durin::PlanSceneImport({
		.RootSource = Bundle.RootSource,
		.DestinationDirectory = MakeAssetPath(
			"/SceneImportTests/SceneImport/ExternalBundle/Gltf"),
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	EXPECT_TRUE(Planned) << Planned.Message;

	Durin::FPreparedSceneSourceBundle FbxBundle;
	ASSERT_TRUE(Durin::PrepareSceneSourceBundle(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/PhongMaterial.fbx",
		Fixture.DestinationDirectory.ToString(),
		"/SceneImportTests/Ingested/PhongMaterial.fbx",
		FbxBundle, Error)) << Error;
	Durin::CommitSceneSourceBundle(FbxBundle);
	const Durin::FSceneImportPlanResult FbxPlanned = Durin::PlanSceneImport({
		.RootSource = FbxBundle.RootSource,
		.DestinationDirectory = MakeAssetPath(
			"/SceneImportTests/SceneImport/ExternalBundle/Fbx"),
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	EXPECT_TRUE(FbxPlanned) << FbxPlanned.Message;
}

TEST(FSceneImportTests, ReimportsManagedPeersInPlaceAndKeepsRecordAuthoritative)
{
	const FSceneFixture Fixture = InitializeSceneFixture("Reimport");
	const Durin::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	const std::string PreviousRecordFingerprint = Initial.Record->GetFingerprint();
	ASSERT_EQ(Initial.Meshes.size(), 1u);
	Durin::DStaticMesh* Mesh = Initial.Meshes[0];
	Durin::DMaterialInstance* Material = Initial.Materials[0];
	Durin::DTexture2D* Texture = Initial.Textures[0];

	const Durin::FSceneImportPlanResult Planned =
		Durin::PlanSceneReimport(*Initial.Record);
	ASSERT_TRUE(Planned) << Planned.Message;
	const Durin::FSceneImportExecutionResult Reimported =
		Durin::ExecuteSceneImport(Planned.Plan);
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_EQ(Reimported.Record, Initial.Record);
	ASSERT_EQ(Reimported.Meshes.size(), 1u);
	EXPECT_EQ(Reimported.Meshes[0], Mesh);
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
	ASSERT_EQ(Initial.Meshes.size(), 1u);
	const std::string MeshFingerprint = Initial.Meshes[0]
		->GetSourceImportData().SourceContentHash;
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
	EXPECT_EQ(Initial.Meshes[0]->GetSourceImportData().SourceContentHash, MeshFingerprint);
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
		MakeAssetPath(Initial.Meshes[0]->GetPackage()->GetPackagePath())));
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
