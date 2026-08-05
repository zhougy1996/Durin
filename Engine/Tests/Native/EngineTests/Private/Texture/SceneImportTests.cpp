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
			Root / "Engine/Content", Root / "Project/Content"})
			std::filesystem::create_directories(Directory);
		auto Mounts = std::make_unique<Durin::PathUtilities::FScopedMountRegistryFixture>(
			std::vector<Durin::PathUtilities::FMountPoint>{
				{
					.VirtualRoot = "/Engine/",
					.Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Engine/Content",
					.bAutoScan = true,
					.bAuthoringWritable = true},
				{
					.VirtualRoot = "/SceneImportTests/",
					.Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Project/Content",
					.bAutoScan = true,
					.bAuthoringWritable = true,
					.Dependencies = {"/Engine/"}}});
		EXPECT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		EXPECT_NE(Durin::EnsureStandardImportedSurfaceMaterial(Error), nullptr)
			<< Error;
		const std::filesystem::path Destination =
			Root / "Project/Content" / "Scenes" / (std::string(Name) + ".gltf");
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

TEST(FSceneImportTests, ImportsGltfPbrFactorsSemanticTexturesAndPackedChannels)
{
	const FSceneFixture Fixture = InitializeSceneFixture("PbrContract");
	Durin::FPreparedSceneSourceBundle Bundle;
	std::string Error;
	ASSERT_TRUE(Durin::PrepareSceneSourceBundle(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/ImportedPbrContract.gltf",
		Fixture.DestinationDirectory.ToString(),
		"/SceneImportTests/Ingested/ImportedPbrContract.gltf",
		Bundle, Error)) << Error;
	Durin::CommitSceneSourceBundle(Bundle);
	const Durin::FSceneImportPlanResult Planned = Durin::PlanSceneImport({
		.RootSource = Bundle.RootSource,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(Planned) << Planned.Message;
	EXPECT_EQ(Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs().size(), 9u);
	EXPECT_TRUE(Planned.Plan.GetWarnings().empty());

	const Durin::FSceneImportExecutionResult Executed =
		Durin::ExecuteSceneImport(Planned.Plan);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_EQ(Executed.Materials.size(), 1u);
	ASSERT_EQ(Executed.Textures.size(), 7u);
	Durin::DMaterialInstance* Material = Executed.Materials[0];
	ASSERT_NE(Material, nullptr);
	EXPECT_TRUE(Material->HasStaticPropertiesOverride());
	EXPECT_EQ(Material->GetStaticProperties().BlendMode, Durin::EMaterialBlendMode::Masked);
	EXPECT_TRUE(Material->GetStaticProperties().bTwoSided);
	EXPECT_FLOAT_EQ(Material->GetStaticProperties().OpacityMaskThreshold, 0.4f);
	auto ExpectScalar = [&](const Durin::FName& Name, float Expected) {
		float Actual = 0.0f;
		ASSERT_TRUE(Material->GetScalarParameterValue(Name, Actual));
		EXPECT_FLOAT_EQ(Actual, Expected);
	};
	ExpectScalar(Durin::MaterialParameters::MetallicName(), 0.25f);
	ExpectScalar(Durin::MaterialParameters::RoughnessName(), 0.75f);
	ExpectScalar(Durin::MaterialParameters::AmbientOcclusionName(), 0.3f);
	ExpectScalar(Durin::MaterialParameters::OpacityName(), 0.5f);
	ExpectScalar(Durin::MaterialParameters::OpacityMaskName(), 0.5f);
	Durin::FVector3 Emissive;
	ASSERT_TRUE(Material->GetVectorParameterValue(
		Durin::MaterialParameters::EmissiveName(), Emissive));
	EXPECT_EQ(Emissive, Durin::FVector3(0.0f));

	const std::array<const Durin::FName*, 8> TextureNames{
		&Durin::MaterialParameters::BaseColorTextureName(),
		&Durin::MaterialParameters::NormalTextureName(),
		&Durin::MaterialParameters::MetallicTextureName(),
		&Durin::MaterialParameters::RoughnessTextureName(),
		&Durin::MaterialParameters::AmbientOcclusionTextureName(),
		&Durin::MaterialParameters::EmissiveTextureName(),
		&Durin::MaterialParameters::OpacityTextureName(),
		&Durin::MaterialParameters::OpacityMaskTextureName()};
	std::array<Durin::DTexture2D*, 8> Textures{};
	for (size_t Role : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
	{
		ASSERT_TRUE(Material->GetTextureParameterValue(*TextureNames[Role], Textures[Role]));
		ASSERT_NE(Textures[Role], nullptr);
	}
	EXPECT_EQ(Textures[0]->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Textures[0]->IsSRGB());
	EXPECT_EQ(Textures[1]->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Textures[1]->IsSRGB());
	for (size_t Role : {2u, 3u, 4u, 7u})
	{
		EXPECT_EQ(Textures[Role]->GetUsage(), Durin::ETextureUsage::DataMask);
		EXPECT_FALSE(Textures[Role]->IsSRGB());
	}
	EXPECT_EQ(Textures[5]->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Textures[5]->IsSRGB());

	const Durin::FTextureSourceData* BasePixels = Textures[0]->GetSourceData();
	const Durin::FTextureSourceData* MetallicPixels = Textures[2]->GetSourceData();
	const Durin::FTextureSourceData* RoughnessPixels = Textures[3]->GetSourceData();
	const Durin::FTextureSourceData* OcclusionPixels = Textures[4]->GetSourceData();
	const Durin::FTextureSourceData* MaskPixels = Textures[7]->GetSourceData();
	ASSERT_NE(BasePixels, nullptr);
	ASSERT_NE(MetallicPixels, nullptr);
	ASSERT_NE(RoughnessPixels, nullptr);
	ASSERT_NE(OcclusionPixels, nullptr);
	ASSERT_NE(MaskPixels, nullptr);
	ASSERT_GE(BasePixels->Pixels.size(), 4u);
	EXPECT_EQ(MetallicPixels->Pixels[0], BasePixels->Pixels[2]);
	EXPECT_EQ(RoughnessPixels->Pixels[0], BasePixels->Pixels[1]);
	EXPECT_EQ(OcclusionPixels->Pixels[0], BasePixels->Pixels[0]);
	EXPECT_EQ(MaskPixels->Pixels[0], BasePixels->Pixels[3]);

	float UVChannel = 0.0f;
	Durin::FVector2 UVScale;
	Durin::FVector2 UVOffset;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorUVChannel"), UVChannel));
	ASSERT_TRUE(Material->GetVector2ParameterValue(Durin::FName("BaseColorUVScale"), UVScale));
	ASSERT_TRUE(Material->GetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), UVOffset));
	EXPECT_FLOAT_EQ(UVChannel, 1.0f);
	EXPECT_EQ(UVScale, Durin::FVector2(2.0f, 3.0f));
	EXPECT_EQ(UVOffset, Durin::FVector2(0.1f, 0.2f));

	std::vector<Durin::DTexture2D*> TextureIdentities = Executed.Textures;
	std::vector<std::string> TextureKeys;
	for (Durin::DTexture2D* Texture : TextureIdentities)
		TextureKeys.push_back(Texture->GetDerivedDataKey());
	const Durin::FSceneImportPlanResult ReimportPlan =
		Durin::PlanSceneReimport(*Executed.Record);
	ASSERT_TRUE(ReimportPlan) << ReimportPlan.Message;
	const Durin::FSceneImportExecutionResult Reimported =
		Durin::ExecuteSceneImport(ReimportPlan.Plan);
	ASSERT_TRUE(Reimported) << Reimported.Message;
	ASSERT_EQ(Reimported.Materials.size(), 1u);
	EXPECT_EQ(Reimported.Materials[0], Material);
	ASSERT_EQ(Reimported.Textures.size(), TextureIdentities.size());
	for (size_t Index = 0; Index < TextureIdentities.size(); ++Index)
	{
		EXPECT_EQ(Reimported.Textures[Index], TextureIdentities[Index]);
		EXPECT_EQ(Reimported.Textures[Index]->GetDerivedDataKey(), TextureKeys[Index]);
	}
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
