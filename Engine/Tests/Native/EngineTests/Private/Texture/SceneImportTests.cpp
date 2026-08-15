#include "TextureTestSupport.h"

#include "Animation/AnimationClip.h"
#include "AssetMutation.h"
#include "ImportRecord.h"
#include "ImportService.h"
#include "Materials/MaterialInstance.h"
#include "SceneImport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "StandardAssetImportProviders.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/Task.h"

namespace
{
	struct FAsyncImportSchedulerGuard
	{
		FAsyncImportSchedulerGuard()
		{
			const Durin::FTaskSchedulerDiagnostics Diagnostics =
				Durin::GetTaskSchedulerDiagnostics();
			bRestoreScheduler = Diagnostics.bRunning;
			PreviousConfig.NumWorkerThreads = Diagnostics.WorkerCount;
			PreviousConfig.MaxNonterminalTasks = Diagnostics.TaskReservationCapacity;
			bRestoreDeferredExecutor =
				Durin::GetGameThreadDeferredWorkQueueDiagnostics().bAccepting;
		}

		~FAsyncImportSchedulerGuard()
		{
			Durin::Asset::Import::GetImportService().CancelAndDrainAllAsyncImports();
			Durin::ShutdownTaskScheduler(false);
			if (bRestoreScheduler && !Durin::InitializeTaskScheduler(PreviousConfig))
			{
				ADD_FAILURE() << "Failed to restore the native-test task scheduler.";
			}
			if (bRestoreDeferredExecutor && !Durin::InitializeGameThreadDeferredExecutor())
			{
				ADD_FAILURE() << "Failed to restore the native-test deferred executor.";
			}
		}

		Durin::FTaskSchedulerConfig PreviousConfig;
		bool bRestoreScheduler = false;
		bool bRestoreDeferredExecutor = false;
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
		EXPECT_TRUE(Durin::Asset::Import::Standard::RegisterStandardAssetImportProviders(
			Error, GetEngineTestModuleCallbackGate())) << Error;
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
		EXPECT_NE(Durin::Asset::Import::Standard::EnsureStandardImportedSurfaceMaterial(Error), nullptr)
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

	auto InitializeSkeletalSceneFixture(
		std::string_view Name,
		std::string_view FixtureFile = "ContractExternal.gltf") -> FSceneFixture
	{
		FSceneFixture Fixture = InitializeSceneFixture(Name);
		const std::filesystem::path SceneDirectory =
			Durin::Testing::GetTestWorkDirectory() / "SceneImport" / std::string(Name)
			/ "Project/Content/Scenes";
		const std::filesystem::path SourceFixture =
			std::filesystem::path(DURIN_TEST_DATA_DIR) / "Skeletal" / FixtureFile;
		const std::string Extension = SourceFixture.extension().generic_string();
		std::filesystem::copy_file(
			SourceFixture,
			SceneDirectory / (std::string(Name) + Extension),
			std::filesystem::copy_options::overwrite_existing);
		Fixture.Source.Path = std::format(
			"/SceneImportTests/Scenes/{}{}", Name, Extension);
		if (FixtureFile == "ContractExternal.gltf")
		{
			std::filesystem::copy_file(
				std::filesystem::path(DURIN_TEST_DATA_DIR) / "Skeletal/Contract.bin",
				SceneDirectory / "Contract.bin",
				std::filesystem::copy_options::overwrite_existing);
		}
		return Fixture;
	}

	auto PlanAndExecute(const FSceneFixture& Fixture, size_t ExpectedOutputCount = 3)
		-> Durin::Asset::Import::Standard::FSceneImportExecutionResult
	{
		const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned = Durin::Asset::Import::Standard::PlanSceneImport({
			.RootSource = Fixture.Source,
			.DestinationDirectory = Fixture.DestinationDirectory,
			.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
		EXPECT_TRUE(Planned) << Planned.Message;
		if (!Planned) return {};
		EXPECT_EQ(
			Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs().size(),
			ExpectedOutputCount);
		return Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan);
	}

}

TEST(FSceneImportTests, PublishesHeterogeneousPeersUnderGenericRecord)
{
	const FSceneFixture Fixture = InitializeSceneFixture("Initial");
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Executed = PlanAndExecute(Fixture);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_NE(Executed.Record, nullptr);
	ASSERT_EQ(Executed.Meshes.size(), 1u);
	ASSERT_EQ(Executed.Materials.size(), 1u);
	ASSERT_EQ(Executed.Textures.size(), 1u);
	EXPECT_EQ(Executed.Record->GetProviderId(), Durin::Asset::Import::Standard::SceneImportProviderId);
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

TEST(FSceneImportTests, PublishesSkeletalAssetGraphAndDeterministicallyReimportsIt)
{
	const FSceneFixture Fixture = InitializeSkeletalSceneFixture("SkeletalGraph");
	const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned = Durin::Asset::Import::Standard::PlanSceneImport({
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(Planned) << Planned.Message;
	const Durin::Asset::Import::FImportPlan& Generic =
		Planned.Plan.GetMultiOutputPlan().GetGenericPlan();
	ASSERT_EQ(Generic.GetOutputs().size(), 11u);
	EXPECT_FALSE(Planned.Plan.GetMultiOutputPlan().GetPrimaryOutput().IsValid());

	std::unordered_map<std::string, size_t> RoleCounts;
	size_t AnimationClipCount = 0;
	for (const Durin::Asset::Import::FImportOutputPreview& Output : Generic.GetOutputs())
	{
		++RoleCounts[Output.Role];
		const std::string Parent = std::filesystem::path(
			Output.AssetPath.ToString()).parent_path().filename().generic_string();
		if (Output.Role == "Skeleton") EXPECT_EQ(Parent, "Skeletons");
		else if (Output.Role == "SkeletalMesh") EXPECT_EQ(Parent, "SkeletalMeshes");
		else if (Output.Role.starts_with("AnimationClip."))
		{
			EXPECT_EQ(Parent, "Animations");
			++AnimationClipCount;
		}
	}
	EXPECT_EQ(RoleCounts["Skeleton"], 2u);
	EXPECT_EQ(RoleCounts["SkeletalMesh"], 2u);
	EXPECT_EQ(AnimationClipCount, 4u);

	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Executed =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_NE(Executed.Record, nullptr);
	ASSERT_EQ(Executed.Record->GetOutputs().size(), 11u);
	EXPECT_FALSE(Executed.Record->GetPrimaryOutput().IsValid());
	EXPECT_EQ(Executed.Record->GetProviderState().SchemaId, "Durin.Scene.ProviderState");
	EXPECT_EQ(Executed.Record->GetProviderState().SchemaVersion, 1u);
	EXPECT_FALSE(Executed.Record->GetProviderState().Bytes.empty());
	ASSERT_EQ(Executed.Skeletons.size(), 2u);
	ASSERT_EQ(Executed.SkeletalMeshes.size(), 2u);
	ASSERT_EQ(Executed.AnimationClips.size(), 4u);
	ASSERT_EQ(Executed.Meshes.size(), 1u);
	ASSERT_EQ(Executed.Materials.size(), 2u);
	EXPECT_TRUE(Executed.Textures.empty());

	std::string Error;
	for (Durin::DSkeleton* Skeleton : Executed.Skeletons)
	{
		ASSERT_NE(Skeleton, nullptr);
		EXPECT_EQ(Skeleton->GetCompatibilityIdentity(),
			"be0f679ef83133e5acfab7f12b688f54");
		EXPECT_TRUE(Skeleton->Validate(Error)) << Error;
	}
	for (size_t MeshIndex = 0; MeshIndex < Executed.SkeletalMeshes.size(); ++MeshIndex)
	{
		Durin::DSkeletalMesh* Mesh = Executed.SkeletalMeshes[MeshIndex];
		ASSERT_NE(Mesh, nullptr);
		EXPECT_EQ(Mesh->GetSkeleton(), Executed.Skeletons[MeshIndex]);
		EXPECT_EQ(Mesh->GetSkeletonCompatibilityIdentity(),
			Executed.Skeletons[MeshIndex]->GetCompatibilityIdentity());
		ASSERT_NE(Mesh->GetPayloadData(), nullptr);
		EXPECT_EQ(Mesh->GetDerivedDataKey().size(), 32u);
		EXPECT_NE(Mesh->GetPayloadStorageDiagnostic().find("Stored"), std::string::npos);
		ASSERT_EQ(Mesh->GetMaterialSlots().size(), 2u);
		for (const Durin::FSkeletalMeshMaterialSlotDefinition& Slot : Mesh->GetMaterialSlots())
			EXPECT_NE(Slot.DefaultMaterial.Get(), nullptr);
		Error.clear();
		EXPECT_TRUE(Mesh->Validate(Error)) << Error;
	}
	const std::array<size_t, 4> ExpectedSkeletonIndices{0u, 1u, 0u, 1u};
	const std::array<std::string_view, 4> ExpectedClipNames{"Walk", "Walk", "Pose", "Pose"};
	for (size_t ClipIndex = 0; ClipIndex < Executed.AnimationClips.size(); ++ClipIndex)
	{
		Durin::DAnimationClip* Clip = Executed.AnimationClips[ClipIndex];
		ASSERT_NE(Clip, nullptr);
		EXPECT_EQ(Clip->GetSkeleton(), Executed.Skeletons[ExpectedSkeletonIndices[ClipIndex]]);
		EXPECT_EQ(Clip->GetClipName(), Durin::FName(ExpectedClipNames[ClipIndex]));
		ASSERT_NE(Clip->GetPayloadData(), nullptr);
		EXPECT_EQ(Clip->GetDerivedDataKey().size(), 32u);
		EXPECT_NE(Clip->GetPayloadStorageDiagnostic().find("Stored"), std::string::npos);
		Error.clear();
		EXPECT_TRUE(Clip->Validate(Error)) << Error;
	}

	const std::string RecordFingerprint = Executed.Record->GetFingerprint();
	const std::vector<Durin::DSkeleton*> SkeletonIdentities = Executed.Skeletons;
	const std::vector<Durin::DSkeletalMesh*> MeshIdentities = Executed.SkeletalMeshes;
	const std::vector<Durin::DAnimationClip*> ClipIdentities = Executed.AnimationClips;
	std::vector<std::string> MeshDerivedDataKeys;
	std::vector<std::string> ClipDerivedDataKeys;
	for (Durin::DSkeletalMesh* Mesh : Executed.SkeletalMeshes)
		MeshDerivedDataKeys.push_back(Mesh->GetDerivedDataKey());
	for (Durin::DAnimationClip* Clip : Executed.AnimationClips)
		ClipDerivedDataKeys.push_back(Clip->GetDerivedDataKey());
	const Durin::Asset::Import::Standard::FSceneImportPlanResult ReimportPlan =
		Durin::Asset::Import::Standard::PlanSceneReimport(*Executed.Record);
	ASSERT_TRUE(ReimportPlan) << ReimportPlan.Message;
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Reimported =
		Durin::Asset::Import::Standard::ExecuteSceneImport(ReimportPlan.Plan);
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_EQ(Reimported.Record, Executed.Record);
	EXPECT_EQ(Reimported.Record->GetFingerprint(), RecordFingerprint);
	EXPECT_EQ(Reimported.Skeletons, SkeletonIdentities);
	EXPECT_EQ(Reimported.SkeletalMeshes, MeshIdentities);
	EXPECT_EQ(Reimported.AnimationClips, ClipIdentities);
	for (size_t Index = 0; Index < Reimported.SkeletalMeshes.size(); ++Index)
		EXPECT_EQ(Reimported.SkeletalMeshes[Index]->GetDerivedDataKey(),
			MeshDerivedDataKeys[Index]);
	for (size_t Index = 0; Index < Reimported.AnimationClips.size(); ++Index)
		EXPECT_EQ(Reimported.AnimationClips[Index]->GetDerivedDataKey(),
			ClipDerivedDataKeys[Index]);
	const Durin::Asset::Import::FImportRecordActionResult ProviderNeutral =
		Durin::Asset::Import::GetImportService().ExecuteImportRecordAction(
			*Reimported.Record,
			Durin::Asset::Import::EImportRecordAction::Reimport);
	ASSERT_TRUE(ProviderNeutral) << ProviderNeutral.Message;
	ASSERT_EQ(ProviderNeutral.Outputs.size(), Reimported.Record->GetOutputs().size());
	for (size_t OutputIndex = 0; OutputIndex < ProviderNeutral.Outputs.size(); ++OutputIndex)
	{
		ASSERT_NE(ProviderNeutral.Outputs[OutputIndex], nullptr);
		EXPECT_EQ(ProviderNeutral.Outputs[OutputIndex]->GetPackage()->GetPackagePath(),
			Reimported.Record->GetOutputs()[OutputIndex].AssetPath.ToString());
	}
}

TEST(FSceneImportTests, GltfAndGlbPublishEquivalentSkeletalGraphsAcrossRepeatedReimport)
{
	std::vector<std::vector<Durin::FSkeletonBone>> ExpectedSkeletons;
	std::vector<Durin::FSkeletalMeshPayloadData> ExpectedMeshes;
	std::vector<Durin::FAnimationClipPayloadData> ExpectedClips;
	const std::array<std::pair<std::string_view, std::string_view>, 2> Cases{{
		{"SkeletalDataUri", "Contract.gltf"},
		{"SkeletalGlb", "Contract.glb"}}};

	for (const auto& [Name, FixtureFile] : Cases)
	{
		SCOPED_TRACE(FixtureFile);
		const FSceneFixture Fixture = InitializeSkeletalSceneFixture(Name, FixtureFile);
		const Durin::Asset::Import::Standard::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture, 11);
		ASSERT_TRUE(Initial) << Initial.Message;
		ASSERT_NE(Initial.Record, nullptr);
		ASSERT_EQ(Initial.Record->GetOutputs().size(), 11u);
		ASSERT_EQ(Initial.Skeletons.size(), 2u);
		ASSERT_EQ(Initial.SkeletalMeshes.size(), 2u);
		ASSERT_EQ(Initial.AnimationClips.size(), 4u);

		std::vector<std::vector<Durin::FSkeletonBone>> Skeletons;
		std::vector<Durin::FSkeletalMeshPayloadData> Meshes;
		std::vector<Durin::FAnimationClipPayloadData> Clips;
		std::vector<std::string> MeshKeys;
		std::vector<std::string> ClipKeys;
		for (Durin::DSkeleton* Skeleton : Initial.Skeletons)
		{
			ASSERT_NE(Skeleton, nullptr);
			const std::span<const Durin::FSkeletonBone> Bones = Skeleton->GetBones();
			Skeletons.emplace_back(Bones.begin(), Bones.end());
		}
		for (Durin::DSkeletalMesh* Mesh : Initial.SkeletalMeshes)
		{
			ASSERT_NE(Mesh, nullptr);
			ASSERT_NE(Mesh->GetPayloadData(), nullptr);
			Meshes.push_back(*Mesh->GetPayloadData());
			MeshKeys.push_back(Mesh->GetDerivedDataKey());
		}
		for (Durin::DAnimationClip* Clip : Initial.AnimationClips)
		{
			ASSERT_NE(Clip, nullptr);
			ASSERT_NE(Clip->GetPayloadData(), nullptr);
			Clips.push_back(*Clip->GetPayloadData());
			ClipKeys.push_back(Clip->GetDerivedDataKey());
		}

		if (ExpectedSkeletons.empty())
		{
			ExpectedSkeletons = Skeletons;
			ExpectedMeshes = Meshes;
			ExpectedClips = Clips;
		}
		else
		{
			EXPECT_EQ(Skeletons, ExpectedSkeletons);
			EXPECT_EQ(Meshes, ExpectedMeshes);
			EXPECT_EQ(Clips, ExpectedClips);
		}

		const std::string RecordFingerprint = Initial.Record->GetFingerprint();
		const std::vector<Durin::DSkeleton*> SkeletonIdentities = Initial.Skeletons;
		const std::vector<Durin::DSkeletalMesh*> MeshIdentities = Initial.SkeletalMeshes;
		const std::vector<Durin::DAnimationClip*> ClipIdentities = Initial.AnimationClips;
		for (size_t ReimportIndex = 0; ReimportIndex < 2; ++ReimportIndex)
		{
			SCOPED_TRACE(std::format("reimport {}", ReimportIndex));
			const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned =
				Durin::Asset::Import::Standard::PlanSceneReimport(*Initial.Record);
			ASSERT_TRUE(Planned) << Planned.Message;
			const Durin::Asset::Import::Standard::FSceneImportExecutionResult Reimported =
				Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan);
			ASSERT_TRUE(Reimported) << Reimported.Message;
			EXPECT_EQ(Reimported.Record, Initial.Record);
			EXPECT_EQ(Reimported.Record->GetFingerprint(), RecordFingerprint);
			EXPECT_EQ(Reimported.Skeletons, SkeletonIdentities);
			EXPECT_EQ(Reimported.SkeletalMeshes, MeshIdentities);
			EXPECT_EQ(Reimported.AnimationClips, ClipIdentities);
			for (size_t Index = 0; Index < Reimported.SkeletalMeshes.size(); ++Index)
			{
				EXPECT_EQ(Reimported.SkeletalMeshes[Index]->GetDerivedDataKey(), MeshKeys[Index]);
				EXPECT_EQ(*Reimported.SkeletalMeshes[Index]->GetPayloadData(), Meshes[Index]);
			}
			for (size_t Index = 0; Index < Reimported.AnimationClips.size(); ++Index)
			{
				EXPECT_EQ(Reimported.AnimationClips[Index]->GetDerivedDataKey(), ClipKeys[Index]);
				EXPECT_EQ(*Reimported.AnimationClips[Index]->GetPayloadData(), Clips[Index]);
			}
		}
	}
}

TEST(FSceneImportTests, SkeletalStaleCollisionPublishesNothing)
{
	const FSceneFixture Fixture = InitializeSkeletalSceneFixture("SkeletalStaleCollision");
	const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned = Durin::Asset::Import::Standard::PlanSceneImport({
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(Planned) << Planned.Message;
	const Durin::Asset::Import::FImportPlan& Generic =
		Planned.Plan.GetMultiOutputPlan().GetGenericPlan();
	const auto SkeletonOutput = std::ranges::find(
		Generic.GetOutputs(), std::string("Skeleton"),
		&Durin::Asset::Import::FImportOutputPreview::Role);
	ASSERT_NE(SkeletonOutput, Generic.GetOutputs().end());
	Durin::DSkeleton* Occupant = nullptr;
	const Durin::Asset::FAssetResult Created =
		Durin::Asset::CreateAsset(SkeletonOutput->AssetPath, Occupant);
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_NE(Occupant, nullptr);
	std::string Error;
	ASSERT_TRUE(Occupant->InitializeCanonicalBones({{
		.Name = Durin::FName("OccupantRoot"),
		.ParentIndex = -1,
		.ReferenceTransform = Durin::FSkeletonTransform{}}}, Error)) << Error;

	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Executed =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan);
	EXPECT_FALSE(Executed);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(
		Planned.Plan.GetMultiOutputPlan().GetRecordPath()), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(
		Planned.Plan.GetMultiOutputPlan().GetRecordPath()), nullptr);
	for (const Durin::Asset::Import::FImportOutputPreview& Output : Generic.GetOutputs())
	{
		Durin::DPackage* Draft = Durin::Asset::FindResidentPackage(Output.AssetPath);
		if (Output.AssetPath == SkeletonOutput->AssetPath)
			EXPECT_EQ(Draft, Occupant->GetPackage());
		else
			EXPECT_EQ(Draft, nullptr);
	}
}

TEST(FSceneImportTests, SkeletalRootLastFailureRestoresEveryPackage)
{
	const FSceneFixture Fixture = InitializeSkeletalSceneFixture("SkeletalFailureRestore");
	const Durin::Asset::Import::Standard::FSceneImportPlanResult InitialPlan = Durin::Asset::Import::Standard::PlanSceneImport({
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(InitialPlan) << InitialPlan.Message;
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Initial =
		Durin::Asset::Import::Standard::ExecuteSceneImport(InitialPlan.Plan);
	ASSERT_TRUE(Initial) << Initial.Message;
	std::vector<std::pair<Durin::DPackage*, std::vector<Durin::uint8>>> BeforeBytes;
	auto Capture = [&](Durin::DObject* Object) {
		ASSERT_NE(Object, nullptr);
		std::vector<Durin::uint8> Bytes;
		const Durin::Asset::FAssetResult Serialized =
			Durin::Asset::SerializeAssetPackageBytes(Object->GetPackage(), Bytes);
		ASSERT_TRUE(Serialized) << Serialized.Message;
		BeforeBytes.emplace_back(Object->GetPackage(), std::move(Bytes));
	};
	Capture(Initial.Record);
	for (Durin::DSkeleton* Skeleton : Initial.Skeletons) Capture(Skeleton);
	for (Durin::DStaticMesh* Mesh : Initial.Meshes) Capture(Mesh);
	for (Durin::DMaterialInstance* Material : Initial.Materials) Capture(Material);
	for (Durin::DSkeletalMesh* Mesh : Initial.SkeletalMeshes) Capture(Mesh);
	for (Durin::DAnimationClip* Clip : Initial.AnimationClips) Capture(Clip);
	ASSERT_EQ(BeforeBytes.size(), 12u);

	const Durin::Asset::Import::Standard::FSceneImportPlanResult ReimportPlan =
		Durin::Asset::Import::Standard::PlanSceneReimport(*Initial.Record);
	ASSERT_TRUE(ReimportPlan) << ReimportPlan.Message;
	Durin::Asset::Import::FMultiOutputExecutionOptions Options;
	Options.SaveOptions.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
	};
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Failed =
		Durin::Asset::Import::Standard::ExecuteSceneImport(ReimportPlan.Plan, Options);
	EXPECT_FALSE(Failed);
	for (const auto& [Package, ExpectedBytes] : BeforeBytes)
	{
		std::vector<Durin::uint8> ActualBytes;
		const Durin::Asset::FAssetResult Serialized =
			Durin::Asset::SerializeAssetPackageBytes(Package, ActualBytes);
		ASSERT_TRUE(Serialized) << Serialized.Message;
		EXPECT_EQ(ActualBytes, ExpectedBytes);
	}
}

TEST(FSceneImportTests, ImportsGltfPbrFactorsSemanticTexturesAndPackedChannels)
{
	const FSceneFixture Fixture = InitializeSceneFixture("PbrContract");
	Durin::Asset::Import::Standard::FPreparedSceneSourceBundle Bundle;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Import::Standard::PrepareSceneSourceBundle(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/ImportedPbrContract.gltf",
		Fixture.DestinationDirectory.ToString(),
		"/SceneImportTests/Ingested/ImportedPbrContract.gltf",
		Bundle, Error)) << Error;
	Durin::Asset::Import::Standard::CommitSceneSourceBundle(Bundle);
	const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned = Durin::Asset::Import::Standard::PlanSceneImport({
		.RootSource = Bundle.RootSource,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(Planned) << Planned.Message;
	EXPECT_EQ(Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs().size(), 9u);
	EXPECT_TRUE(Planned.Plan.GetWarnings().empty());

	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Executed =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan);
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
	float UVRotation = 0.0f;
	float SamplerState = 0.0f;
	Durin::FVector2 UVScale;
	Durin::FVector2 UVOffset;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorUVChannel"), UVChannel));
	ASSERT_TRUE(Material->GetVector2ParameterValue(Durin::FName("BaseColorUVScale"), UVScale));
	ASSERT_TRUE(Material->GetVector2ParameterValue(Durin::FName("BaseColorUVOffset"), UVOffset));
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorUVRotation"), UVRotation));
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorSamplerState"), SamplerState));
	EXPECT_FLOAT_EQ(UVChannel, 1.0f);
	EXPECT_EQ(UVScale, Durin::FVector2(2.0f, 3.0f));
	EXPECT_EQ(UVOffset, Durin::FVector2(0.1f, 0.2f));
	EXPECT_FLOAT_EQ(UVRotation, 0.5f);
	Durin::FMaterialSamplerState DecodedSampler;
	ASSERT_TRUE(Durin::TryDecodeMaterialSamplerState(SamplerState, DecodedSampler));
	EXPECT_EQ(DecodedSampler.MinFilter, Durin::EMaterialSamplerMinFilter::NearestMipmapNearest);
	EXPECT_EQ(DecodedSampler.MagFilter, Durin::EMaterialSamplerMagFilter::Nearest);
	EXPECT_EQ(DecodedSampler.AddressU, Durin::EMaterialSamplerAddressMode::MirroredRepeat);
	EXPECT_EQ(DecodedSampler.AddressV, Durin::EMaterialSamplerAddressMode::ClampToEdge);

	std::vector<Durin::DTexture2D*> TextureIdentities = Executed.Textures;
	std::vector<std::string> TextureKeys;
	for (Durin::DTexture2D* Texture : TextureIdentities)
		TextureKeys.push_back(Texture->GetDerivedDataKey());
	const Durin::Asset::Import::Standard::FSceneImportPlanResult ReimportPlan =
		Durin::Asset::Import::Standard::PlanSceneReimport(*Executed.Record);
	ASSERT_TRUE(ReimportPlan) << ReimportPlan.Message;
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Reimported =
		Durin::Asset::Import::Standard::ExecuteSceneImport(ReimportPlan.Plan);
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
	const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned = Durin::Asset::Import::Standard::PlanSceneImport({
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(Planned) << Planned.Message;

	const Durin::Asset::Import::FImportPlan& Generic =
		Planned.Plan.GetMultiOutputPlan().GetGenericPlan();
	ASSERT_EQ(Generic.GetOutputs().size(), 3u);
	for (const Durin::Asset::Import::FImportOutputPreview& Output : Generic.GetOutputs())
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
	FAsyncImportSchedulerGuard SchedulerGuard;
	Durin::ShutdownTaskScheduler(false);
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const FSceneFixture Fixture = InitializeSceneFixture("AsyncEquivalence");
	Durin::Asset::Import::Standard::FSceneImportRequest Request{
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()};
	Durin::Asset::Import::Standard::FSceneImportPlanResult Synchronous = Durin::Asset::Import::Standard::PlanSceneImport(Request);
	ASSERT_TRUE(Synchronous) << Synchronous.Message;
	Durin::Asset::Import::Standard::FSceneImportExecutionResult Initial =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Synchronous.Plan);
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
	Durin::Asset::Import::Standard::FSceneImportAsyncPlanHandle Handle = Durin::Asset::Import::Standard::BeginSceneImportPlan(
		Request, "Tests.SceneImport.AsyncEquivalence");
	ASSERT_TRUE(Handle);
	Durin::Asset::Import::Standard::FSceneImportPlanResult Asynchronous;
	Durin::Asset::Import::EAsyncImportPlanStatus Status =
		Durin::Asset::Import::EAsyncImportPlanStatus::Pending;
	for (Durin::uint32 Attempt = 0; Attempt < 10'000
		&& Status == Durin::Asset::Import::EAsyncImportPlanStatus::Pending; ++Attempt)
	{
		Status = Durin::Asset::Import::Standard::PollSceneImportPlan(Handle, Asynchronous);
		if (Status == Durin::Asset::Import::EAsyncImportPlanStatus::Pending)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(Status, Durin::Asset::Import::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Asynchronous) << Asynchronous.Message;
	const auto& SyncGeneric =
		Synchronous.Plan.GetMultiOutputPlan().GetGenericPlan();
	const auto& AsyncGeneric =
		Asynchronous.Plan.GetMultiOutputPlan().GetGenericPlan();
	EXPECT_EQ(SyncGeneric.GetFingerprint(), AsyncGeneric.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(
		SyncGeneric.GetOutputs(), AsyncGeneric.GetOutputs()));
	EXPECT_EQ(Synchronous.Diagnostics, Asynchronous.Diagnostics);
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Canceled = Durin::Asset::Import::Standard::ExecuteSceneImport(
		Asynchronous.Plan, {
			.IsCancellationRequested = [] { return true; }});
	ASSERT_FALSE(Canceled);
	ASSERT_FALSE(Canceled.Diagnostics.empty());
	EXPECT_EQ(Canceled.Diagnostics.back().Category,
		Durin::Asset::Import::EImportDiagnosticCategory::Canceled);
	Durin::Asset::Import::Standard::FSceneImportExecutionResult Reimported =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Asynchronous.Plan);
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

TEST(FSceneImportTests, SkeletalAsyncPreparationMatchesSynchronousAssetGraph)
{
	FAsyncImportSchedulerGuard SchedulerGuard;
	Durin::ShutdownTaskScheduler(false);
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const FSceneFixture Fixture = InitializeSkeletalSceneFixture("SkeletalAsyncEquivalence");
	const Durin::Asset::Import::Standard::FSceneImportRequest Request{
		.RootSource = Fixture.Source,
		.DestinationDirectory = Fixture.DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()};
	const Durin::Asset::Import::Standard::FSceneImportPlanResult Synchronous = Durin::Asset::Import::Standard::PlanSceneImport(Request);
	ASSERT_TRUE(Synchronous) << Synchronous.Message;

	Durin::Asset::Import::Standard::FSceneImportAsyncPlanHandle Handle = Durin::Asset::Import::Standard::BeginSceneImportPlan(
		Request, "Tests.SceneImport.SkeletalAsyncEquivalence");
	ASSERT_TRUE(Handle);
	Durin::Asset::Import::Standard::FSceneImportPlanResult Asynchronous;
	Durin::Asset::Import::EAsyncImportPlanStatus Status =
		Durin::Asset::Import::EAsyncImportPlanStatus::Pending;
	for (Durin::uint32 Attempt = 0; Attempt < 10'000
		&& Status == Durin::Asset::Import::EAsyncImportPlanStatus::Pending; ++Attempt)
	{
		Status = Durin::Asset::Import::Standard::PollSceneImportPlan(Handle, Asynchronous);
		if (Status == Durin::Asset::Import::EAsyncImportPlanStatus::Pending)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_EQ(Status, Durin::Asset::Import::EAsyncImportPlanStatus::Succeeded);
	ASSERT_TRUE(Asynchronous) << Asynchronous.Message;
	const Durin::Asset::Import::FImportPlan& SyncGeneric =
		Synchronous.Plan.GetMultiOutputPlan().GetGenericPlan();
	const Durin::Asset::Import::FImportPlan& AsyncGeneric =
		Asynchronous.Plan.GetMultiOutputPlan().GetGenericPlan();
	EXPECT_EQ(SyncGeneric.GetFingerprint(), AsyncGeneric.GetFingerprint());
	EXPECT_TRUE(std::ranges::equal(SyncGeneric.GetOutputs(), AsyncGeneric.GetOutputs()));
	EXPECT_EQ(Synchronous.Diagnostics, Asynchronous.Diagnostics);

	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Executed =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Asynchronous.Plan);
	ASSERT_TRUE(Executed) << Executed.Message;
	EXPECT_EQ(Executed.Skeletons.size(), 2u);
	EXPECT_EQ(Executed.SkeletalMeshes.size(), 2u);
	EXPECT_EQ(Executed.AnimationClips.size(), 4u);
}

TEST(FSceneImportTests, UsesProviderNeutralRecordCapabilitiesForReimport)
{
	const FSceneFixture Fixture = InitializeSceneFixture("RecordCapabilities");
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	Durin::FAssetPath RecordPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Initial.Record->GetPackage()->GetPackagePath(), RecordPath));
	const auto Inspection = Durin::Asset::Import::InspectImportRecord(
		RecordPath, Durin::Asset::Import::GetImportRecordIndex());
	ASSERT_TRUE(Inspection) << Inspection.Message;
	const auto Capabilities = Durin::Asset::Import::GetImportService()
		.QueryImportRecordCapabilities(Inspection);
	const auto* Reimport = Capabilities.Find(
		Durin::Asset::Import::EImportRecordAction::Reimport);
	ASSERT_NE(Reimport, nullptr);
	ASSERT_TRUE(Reimport->bAvailable);
	const auto Executed = Durin::Asset::Import::GetImportService().ExecuteImportRecordAction(
		*Inspection.Record,
		Durin::Asset::Import::EImportRecordAction::Reimport);
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
	Durin::Asset::Import::Standard::FPreparedSceneSourceBundle Bundle;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Import::Standard::PrepareSceneSourceBundle(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/MaterialContract.gltf",
		Fixture.DestinationDirectory.ToString(),
		"/SceneImportTests/Ingested/MaterialContract.gltf",
		Bundle, Error)) << Error;
	ASSERT_EQ(Bundle.Sources.size(), 3u);
	Durin::Asset::Import::Standard::CommitSceneSourceBundle(Bundle);
	const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned = Durin::Asset::Import::Standard::PlanSceneImport({
		.RootSource = Bundle.RootSource,
		.DestinationDirectory = MakeAssetPath(
			"/SceneImportTests/SceneImport/ExternalBundle/Gltf"),
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	EXPECT_TRUE(Planned) << Planned.Message;

	Durin::Asset::Import::Standard::FPreparedSceneSourceBundle FbxBundle;
	ASSERT_TRUE(Durin::Asset::Import::Standard::PrepareSceneSourceBundle(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/PhongMaterial.fbx",
		Fixture.DestinationDirectory.ToString(),
		"/SceneImportTests/Ingested/PhongMaterial.fbx",
		FbxBundle, Error)) << Error;
	Durin::Asset::Import::Standard::CommitSceneSourceBundle(FbxBundle);
	const Durin::Asset::Import::Standard::FSceneImportPlanResult FbxPlanned = Durin::Asset::Import::Standard::PlanSceneImport({
		.RootSource = FbxBundle.RootSource,
		.DestinationDirectory = MakeAssetPath(
			"/SceneImportTests/SceneImport/ExternalBundle/Fbx"),
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	EXPECT_TRUE(FbxPlanned) << FbxPlanned.Message;
}

TEST(FSceneImportTests, ReimportsManagedPeersInPlaceAndKeepsRecordAuthoritative)
{
	const FSceneFixture Fixture = InitializeSceneFixture("Reimport");
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	const std::string PreviousRecordFingerprint = Initial.Record->GetFingerprint();
	ASSERT_EQ(Initial.Meshes.size(), 1u);
	Durin::DStaticMesh* Mesh = Initial.Meshes[0];
	Durin::DMaterialInstance* Material = Initial.Materials[0];
	Durin::DTexture2D* Texture = Initial.Textures[0];

	const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned =
		Durin::Asset::Import::Standard::PlanSceneReimport(*Initial.Record);
	ASSERT_TRUE(Planned) << Planned.Message;
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Reimported =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan);
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
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	const std::string RecordFingerprint = Initial.Record->GetFingerprint();
	ASSERT_EQ(Initial.Meshes.size(), 1u);
	const std::string MeshFingerprint = Initial.Meshes[0]
		->GetSourceImportData().SourceContentHash;
	const std::string TextureKey = Initial.Textures[0]->GetDerivedDataKey();

	const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned =
		Durin::Asset::Import::Standard::PlanSceneReimport(*Initial.Record);
	ASSERT_TRUE(Planned) << Planned.Message;
	Durin::Asset::Import::FMultiOutputExecutionOptions Options;
	Options.SaveOptions.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::PublishRootPackage;
	};
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Failed =
		Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan, Options);
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Initial.Record->GetFingerprint(), RecordFingerprint);
	EXPECT_EQ(Initial.Meshes[0]->GetSourceImportData().SourceContentHash, MeshFingerprint);
	EXPECT_EQ(Initial.Textures[0]->GetDerivedDataKey(), TextureKey);
}

TEST(FSceneImportTests, RecordReloadDoesNotLoadOutputDependencyClosure)
{
	const FSceneFixture Fixture = InitializeSceneFixture("RecordReload");
	const Durin::Asset::Import::Standard::FSceneImportExecutionResult Initial = PlanAndExecute(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	Durin::FAssetPath RecordPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Initial.Record->GetPackage()->GetPackagePath(), RecordPath));
	std::vector<Durin::FAssetPath> Outputs;
	for (const Durin::Asset::Import::FImportRecordOutput& Output
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

	Durin::Asset::Import::DImportRecord* Reloaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(RecordPath, Reloaded));
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetOutputs().size(), Outputs.size());
	for (const Durin::FAssetPath& Output : Outputs)
		EXPECT_EQ(Durin::Asset::FindResidentPackage(Output), nullptr);
}
