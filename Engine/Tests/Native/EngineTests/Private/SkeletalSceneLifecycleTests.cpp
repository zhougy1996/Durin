#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "Components/SkeletalMeshComponent.h"
#include "AssetSystem.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "SceneImport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "SkeletalMesh/Skeleton.h"
#include "StandardAssetImportProviders.h"
#include "StandardAssetAuthoringTestSupport.h"
#include "Thumbnail/SkeletalMeshAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"

namespace
{
	auto MakeAssetPath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Result;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Result));
		return Result;
	}

	auto ShutdownAssetManager() -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
	}

	auto InitializeAssetManager() -> void
	{
		Durin::Asset::FAssetManager::Get().Initialize();
	}

	using FCookTree = std::vector<std::pair<std::string, std::vector<Durin::uint8>>>;

	auto ReadCookTree(const std::filesystem::path& Root, FCookTree& Out) -> bool
	{
		for (const std::filesystem::directory_entry& Entry :
			std::filesystem::recursive_directory_iterator(Root))
		{
			if (!Entry.is_regular_file()) continue;
			std::vector<Durin::uint8> Bytes;
			if (!Durin::FFileHelper::LoadFileToArray(
				Bytes, Entry.path().generic_string())) return false;
			Out.emplace_back(
				std::filesystem::relative(Entry.path(), Root).generic_string(),
				std::move(Bytes));
		}
		std::ranges::sort(Out, {}, [](const auto& Entry) -> const std::string& {
			return Entry.first;
		});
		return true;
	}

	auto AddPackageOnly(
		Durin::Asset::FCookContext& Context,
		Durin::DObject& Object,
		std::string& OutError) -> bool
	{
		std::vector<Durin::uint8> Bytes;
		const Durin::Asset::FAssetResult Serialized =
			Durin::Asset::SerializeAssetPackageBytes(Object.GetPackage(), Bytes);
		if (!Serialized)
		{
			OutError = Serialized.Message;
			return false;
		}
		return Context.AddPackage(
			Object.GetPackage()->GetPackagePath(), std::move(Bytes), {}, &OutError);
	}
}

TEST(FSkeletalSceneLifecycleTests, GltfAndGlbCookDeterministicallyAndLoadRuntimeOnly)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "SkeletalSceneLifecycle";
	const std::filesystem::path EngineContent = Root / "Engine/Content";
	const std::filesystem::path GameContent = Root / "Game/Content";
	const std::filesystem::path CacheRoot = Root / "DerivedDataCache";
	const std::string PreviousDerivedDataCache =
		Durin::FPaths::DerivedDataCacheDir();
	const std::filesystem::path FirstCookRoot =
		std::filesystem::absolute(Root / "CookFirst");
	const std::filesystem::path SecondCookRoot =
		std::filesystem::absolute(Root / "CookSecond");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(EngineContent);
	std::filesystem::create_directories(GameContent / "Scenes");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());

	std::vector<Durin::FAssetPath> MeshPaths;
	std::vector<Durin::FSkeletalMeshPayloadData> ExpectedMeshes;
	std::vector<Durin::FAssetPath> ClipPaths;
	std::vector<Durin::FAnimationClipPayloadData> ExpectedClips;
	std::vector<Durin::FAssetPath> RecordPaths;
	{
		const std::array<Durin::PathUtilities::FMountPoint, 2> MountDefinitions{{
			{
				.VirtualRoot = "/Engine/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = EngineContent,
				.bAutoScan = true,
				.bAuthoringWritable = true},
			{
				.VirtualRoot = "/Game/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = GameContent,
				.bAutoScan = true,
				.bAuthoringWritable = true,
				.Dependencies = {"/Engine/"}}}};
		Durin::PathUtilities::FScopedMountRegistryFixture Mounts(MountDefinitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		std::string Error;
		ASSERT_TRUE(Durin::Tests::InstallStandardAssetAuthoringFeatures());
		ASSERT_TRUE(Durin::Asset::Import::Standard::RegisterStandardAssetImportProviders(
			Error, GetEngineTestModuleCallbackGate())) << Error;
		Durin::DMaterial* StandardMaterial =
			Durin::Asset::Import::Standard::EnsureStandardImportedSurfaceMaterial(Error);
		ASSERT_NE(StandardMaterial, nullptr) << Error;

		const std::array<std::pair<std::string_view, std::string_view>, 2> Cases{{
			{"DataUri", "Contract.gltf"},
			{"Binary", "Contract.glb"}}};
		std::array<Durin::Asset::Import::Standard::FSceneImportExecutionResult, 2> Results;
		std::vector<std::vector<Durin::FSkeletonBone>> ReferenceSkeletons;
		std::vector<Durin::FSkeletalMeshPayloadData> ReferenceMeshes;
		std::vector<Durin::FAnimationClipPayloadData> ReferenceClips;
		for (size_t CaseIndex = 0; CaseIndex < Cases.size(); ++CaseIndex)
		{
			const auto& [Name, FixtureFile] = Cases[CaseIndex];
			SCOPED_TRACE(FixtureFile);
			const std::filesystem::path SourceFixture =
				std::filesystem::path(DURIN_TEST_DATA_DIR) / "Skeletal" / FixtureFile;
			const std::string Extension = SourceFixture.extension().generic_string();
			std::filesystem::copy_file(
				SourceFixture, GameContent / "Scenes" / (std::string(Name) + Extension),
				std::filesystem::copy_options::overwrite_existing);
			const Durin::Asset::Import::Standard::FSceneImportPlanResult Planned = Durin::Asset::Import::Standard::PlanSceneImport({
				.RootSource = {.Path = std::format("/Game/Scenes/{}{}", Name, Extension)},
				.DestinationDirectory = MakeAssetPath(
					std::format("/Game/Imports/{}", Name)),
				.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
			ASSERT_TRUE(Planned) << Planned.Message;
			ASSERT_EQ(
				Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs().size(), 11u);
			Results[CaseIndex] = Durin::Asset::Import::Standard::ExecuteSceneImport(Planned.Plan);
			const Durin::Asset::Import::Standard::FSceneImportExecutionResult& Initial = Results[CaseIndex];
			ASSERT_TRUE(Initial) << Initial.Message;
			ASSERT_EQ(Initial.Skeletons.size(), 2u);
			ASSERT_EQ(Initial.SkeletalMeshes.size(), 2u);
			ASSERT_EQ(Initial.AnimationClips.size(), 4u);
			ASSERT_EQ(Initial.Materials.size(), 2u);
			EXPECT_TRUE(Initial.Textures.empty());
			for (Durin::DSkeletalMesh* Mesh : Initial.SkeletalMeshes)
			{
				const Durin::FAssetPath MeshPath =
					MakeAssetPath(Mesh->GetPackage()->GetPackagePath());
				const Durin::Asset::FAssetData* MeshData =
					Durin::Asset::GetAssetRegistry().FindAssetExact(MeshPath);
				ASSERT_NE(MeshData, nullptr);
				Durin::Editor::SkeletalMesh::FSkeletalMeshAssetThumbnailProvider Provider;
				Durin::Editor::FAssetThumbnailGenerationRequest ThumbnailRequest;
				const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint{
					.VirtualPath = MeshData->PackagePath,
					.AssetClassName = MeshData->AssetClassName,
					.PackageFormatVersion = MeshData->FormatVersion,
					.FileSize = static_cast<Durin::uint64>(MeshData->FileSize),
					.LastWriteTimeTicks = MeshData->LastWriteTimeTicks};
				ASSERT_TRUE(Provider.CaptureGenerationRequest({
					.Asset = Fingerprint,
					.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
					.RequestSerial = 1}, 7, ThumbnailRequest, Error)) << Error;
				EXPECT_TRUE(std::ranges::any_of(
					ThumbnailRequest.KeyInput.Dependencies,
					[Mesh](const Durin::Editor::FAssetThumbnailPackageFingerprint& Dependency) {
						return Mesh->GetSkeleton()
							&& Dependency.VirtualPath.GetView()
								== Mesh->GetSkeleton()->GetPackage()->GetPackagePath();
					}));
				EXPECT_FALSE(std::static_pointer_cast<const
					Durin::Editor::SkeletalMesh::FSkeletalMeshAssetThumbnailGenerationInput>(
						ThumbnailRequest.Input)->Visual.bOutputOpaque);
			}

			std::vector<std::vector<Durin::FSkeletonBone>> Skeletons;
			std::vector<Durin::FSkeletalMeshPayloadData> Meshes;
			std::vector<Durin::FAnimationClipPayloadData> Clips;
			for (Durin::DSkeleton* Skeleton : Initial.Skeletons)
			{
				const std::span<const Durin::FSkeletonBone> Bones = Skeleton->GetBones();
				Skeletons.emplace_back(Bones.begin(), Bones.end());
			}
			for (Durin::DSkeletalMesh* Mesh : Initial.SkeletalMeshes)
			{
				ASSERT_NE(Mesh->GetPayloadData(), nullptr);
				ASSERT_NE(Mesh->GetRenderData(), nullptr);
				EXPECT_EQ(Mesh->GetRenderData()->LODIndex, 0u);
				EXPECT_EQ(Mesh->GetRenderData()->IndexBuffer.GetIndices(),
					Mesh->GetPayloadData()->Indices);
				Meshes.push_back(*Mesh->GetPayloadData());
			}
			for (Durin::DAnimationClip* Clip : Initial.AnimationClips)
			{
				ASSERT_NE(Clip->GetPayloadData(), nullptr);
				Clips.push_back(*Clip->GetPayloadData());
			}
			if (CaseIndex == 0)
			{
				ReferenceSkeletons = Skeletons;
				ReferenceMeshes = Meshes;
				ReferenceClips = Clips;
			}
			else
			{
				EXPECT_EQ(Skeletons, ReferenceSkeletons);
				EXPECT_EQ(Meshes, ReferenceMeshes);
				EXPECT_EQ(Clips, ReferenceClips);
			}

			const std::string RecordFingerprint = Initial.Record->GetFingerprint();
			const std::vector<Durin::DSkeleton*> SkeletonIdentities = Initial.Skeletons;
			const std::vector<Durin::DSkeletalMesh*> MeshIdentities =
				Initial.SkeletalMeshes;
			const std::vector<Durin::DAnimationClip*> ClipIdentities =
				Initial.AnimationClips;
			std::vector<std::string> MeshKeys;
			std::vector<std::string> ClipKeys;
			for (Durin::DSkeletalMesh* Mesh : Initial.SkeletalMeshes)
				MeshKeys.push_back(Mesh->GetDerivedDataKey());
			for (Durin::DAnimationClip* Clip : Initial.AnimationClips)
				ClipKeys.push_back(Clip->GetDerivedDataKey());
			for (size_t ReimportIndex = 0; ReimportIndex < 2; ++ReimportIndex)
			{
				const Durin::Asset::Import::Standard::FSceneImportPlanResult ReimportPlan =
					Durin::Asset::Import::Standard::PlanSceneReimport(*Initial.Record);
				ASSERT_TRUE(ReimportPlan) << ReimportPlan.Message;
				const Durin::Asset::Import::Standard::FSceneImportExecutionResult Reimported =
					Durin::Asset::Import::Standard::ExecuteSceneImport(ReimportPlan.Plan);
				ASSERT_TRUE(Reimported) << Reimported.Message;
				EXPECT_EQ(Reimported.Record, Initial.Record);
				EXPECT_EQ(Reimported.Record->GetFingerprint(), RecordFingerprint);
				EXPECT_EQ(Reimported.Skeletons, SkeletonIdentities);
				EXPECT_EQ(Reimported.SkeletalMeshes, MeshIdentities);
				EXPECT_EQ(Reimported.AnimationClips, ClipIdentities);
				for (size_t Index = 0; Index < MeshIdentities.size(); ++Index)
					EXPECT_EQ(
						Reimported.SkeletalMeshes[Index]->GetDerivedDataKey(), MeshKeys[Index]);
				for (size_t Index = 0; Index < ClipIdentities.size(); ++Index)
					EXPECT_EQ(
						Reimported.AnimationClips[Index]->GetDerivedDataKey(), ClipKeys[Index]);
			}
		}

		auto Cook = [&](const std::filesystem::path& CookRoot) -> bool {
			Durin::Asset::FCookContext Context(
				CookRoot, Durin::Asset::ECookTargetPlatform::Win64,
				Durin::Asset::ECookTargetProfile::Game);
			if (!AddPackageOnly(Context, *StandardMaterial, Error)) return false;
			for (const Durin::Asset::Import::Standard::FSceneImportExecutionResult& Result : Results)
			{
				for (Durin::DMaterialInstance* Material : Result.Materials)
					if (!AddPackageOnly(Context, *Material, Error)) return false;
				for (Durin::DSkeleton* Skeleton : Result.Skeletons)
					if (!Skeleton->AddToCook(
						Context, Skeleton->GetPackage()->GetPackagePath(), Error)) return false;
				for (Durin::DSkeletalMesh* Mesh : Result.SkeletalMeshes)
					if (!Mesh->AddToCook(
						Context, Mesh->GetPackage()->GetPackagePath(), Error)) return false;
				for (Durin::DAnimationClip* Clip : Result.AnimationClips)
					if (!Clip->AddToCook(
						Context, Clip->GetPackage()->GetPackagePath(), Error)) return false;
			}
			return Context.Publish(&Error);
		};
		ASSERT_TRUE(Cook(FirstCookRoot)) << Error;
		ASSERT_TRUE(Cook(SecondCookRoot)) << Error;

		FCookTree FirstTree;
		FCookTree SecondTree;
		ASSERT_TRUE(ReadCookTree(FirstCookRoot, FirstTree));
		ASSERT_TRUE(ReadCookTree(SecondCookRoot, SecondTree));
		EXPECT_FALSE(FirstTree.empty());
		EXPECT_EQ(FirstTree, SecondTree);
		for (const Durin::Asset::Import::Standard::FSceneImportExecutionResult& Result : Results)
		{
			RecordPaths.push_back(MakeAssetPath(
				Result.Record->GetPackage()->GetPackagePath()));
			for (Durin::DSkeletalMesh* Mesh : Result.SkeletalMeshes)
			{
				MeshPaths.push_back(MakeAssetPath(Mesh->GetPackage()->GetPackagePath()));
				ExpectedMeshes.push_back(*Mesh->GetPayloadData());
			}
			for (Durin::DAnimationClip* Clip : Result.AnimationClips)
			{
				ClipPaths.push_back(MakeAssetPath(Clip->GetPackage()->GetPackagePath()));
				ExpectedClips.push_back(*Clip->GetPayloadData());
			}
		}
		ShutdownAssetManager();
	}

	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	InitializeAssetManager();
	{
		const std::array<Durin::PathUtilities::FMountPoint, 2> MountDefinitions{{
			{
				.VirtualRoot = "/Engine/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = EngineContent,
				.bAutoScan = true,
				.bAuthoringWritable = true},
			{
				.VirtualRoot = "/Game/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = GameContent,
				.bAutoScan = true,
				.bAuthoringWritable = true,
				.Dependencies = {"/Engine/"}}}};
		Durin::PathUtilities::FScopedMountRegistryFixture Mounts(MountDefinitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		for (const Durin::FAssetPath& RecordPath : RecordPaths)
		{
			Durin::Asset::Import::DImportRecord* Record = nullptr;
			ASSERT_TRUE(Durin::Asset::LoadAsset(RecordPath, Record));
			ASSERT_NE(Record, nullptr);
			const Durin::Asset::Import::Standard::FSceneImportPlanResult ReimportPlan =
				Durin::Asset::Import::Standard::PlanSceneReimport(*Record);
			ASSERT_TRUE(ReimportPlan) << ReimportPlan.Message;
			const Durin::Asset::Import::Standard::FSceneImportExecutionResult Reimported =
				Durin::Asset::Import::Standard::ExecuteSceneImport(ReimportPlan.Plan);
			ASSERT_TRUE(Reimported) << Reimported.Message;
			for (Durin::DSkeletalMesh* Mesh : Reimported.SkeletalMeshes)
				EXPECT_NE(Mesh->GetPayloadData(), nullptr);
			for (Durin::DAnimationClip* Clip : Reimported.AnimationClips)
				EXPECT_NE(Clip->GetPayloadData(), nullptr);
		}
		EXPECT_TRUE(std::filesystem::exists(CacheRoot / "SkeletalMesh/Objects"));
		EXPECT_TRUE(std::filesystem::exists(CacheRoot / "AnimationClip/Objects"));
		ShutdownAssetManager();
		Durin::CollectGarbage();
	}

	Durin::Testing::RemoveTestWorkDirectory(EngineContent);
	Durin::Testing::RemoveTestWorkDirectory(GameContent);
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	EXPECT_FALSE(std::filesystem::exists(GameContent / "Scenes/DataUri.gltf"));
	EXPECT_FALSE(std::filesystem::exists(GameContent / "Scenes/Binary.glb"));
	EXPECT_FALSE(std::filesystem::exists(CacheRoot));
	InitializeAssetManager();
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::CookedRuntime, FirstCookRoot}));
	{
		const std::array<Durin::PathUtilities::FMountPoint, 2> MountDefinitions{{
			{
				.VirtualRoot = "/Engine/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = FirstCookRoot / "Engine",
				.bAutoScan = true},
			{
				.VirtualRoot = "/Game/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = FirstCookRoot / "Game",
				.bAutoScan = true,
				.Dependencies = {"/Engine/"}}}};
		Durin::PathUtilities::FScopedMountRegistryFixture Mounts(MountDefinitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		std::vector<Durin::DSkeletalMesh*> RuntimeMeshes;
		RuntimeMeshes.reserve(MeshPaths.size());
		for (size_t Index = 0; Index < MeshPaths.size(); ++Index)
		{
			Durin::DSkeletalMesh* Mesh = nullptr;
			ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPaths[Index], Mesh));
			ASSERT_NE(Mesh, nullptr);
			ASSERT_NE(Mesh->GetSkeleton(), nullptr);
			ASSERT_NE(Mesh->GetPayloadData(), nullptr);
			ASSERT_NE(Mesh->GetRenderData(), nullptr);
			EXPECT_EQ(Mesh->GetRenderData()->IndexBuffer.GetIndices(),
				Mesh->GetPayloadData()->Indices);
			EXPECT_EQ(*Mesh->GetPayloadData(), ExpectedMeshes[Index]);
			EXPECT_TRUE(Mesh->GetDerivedDataKey().empty());
			RuntimeMeshes.push_back(Mesh);
		}
		std::vector<Durin::DAnimationClip*> RuntimeClips;
		RuntimeClips.reserve(ClipPaths.size());
		for (size_t Index = 0; Index < ClipPaths.size(); ++Index)
		{
			Durin::DAnimationClip* Clip = nullptr;
			ASSERT_TRUE(Durin::Asset::LoadAsset(ClipPaths[Index], Clip));
			ASSERT_NE(Clip, nullptr);
			ASSERT_NE(Clip->GetSkeleton(), nullptr);
			ASSERT_NE(Clip->GetPayloadData(), nullptr);
			EXPECT_EQ(*Clip->GetPayloadData(), ExpectedClips[Index]);
			EXPECT_TRUE(Clip->GetDerivedDataKey().empty());
			RuntimeClips.push_back(Clip);
		}

		std::vector<std::shared_ptr<const Durin::FSkeletalPosePalette>> RuntimePoses;
		for (size_t MeshIndex = 0; MeshIndex < RuntimeMeshes.size(); ++MeshIndex)
		{
			Durin::DSkeletalMesh* Mesh = RuntimeMeshes[MeshIndex];
			const auto CaseClipBegin = RuntimeClips.begin() + static_cast<std::ptrdiff_t>(MeshIndex / 2 * 4);
			const auto CaseClipEnd = CaseClipBegin + 4;
			const auto ClipIt = std::find_if(CaseClipBegin, CaseClipEnd, [Mesh](const Durin::DAnimationClip* Clip) {
				return Clip->GetSkeletonCompatibilityIdentity()
					== Mesh->GetSkeletonCompatibilityIdentity();
			});
			ASSERT_NE(ClipIt, CaseClipEnd);
			auto* Component = Durin::NewObject<Durin::DSkeletalMeshComponent>(
				nullptr, Durin::FName(std::format("RuntimeSkeletalComponent{}", MeshIndex)));
			std::string Error;
			ASSERT_TRUE(Component->SetSkeletalMesh(Mesh, Error)) << Error;
			ASSERT_TRUE(Component->SetAnimationClip(*ClipIt, Error)) << Error;
			Component->RegisterComponent();
			Component->DispatchBeginPlay();
			ASSERT_TRUE(Component->Seek(1.0f, Error)) << Error;
			const auto Pose = Component->GetLatestPosePalette();
			ASSERT_NE(Pose, nullptr);
			EXPECT_FLOAT_EQ(Pose->SampleTimeSeconds, 1.0f);
			EXPECT_EQ(Pose->SkeletonCompatibilityIdentity,
				Mesh->GetSkeletonCompatibilityIdentity());
			EXPECT_EQ(Pose->Matrices.size(), Mesh->GetPayloadData()->PaletteBoneIndices.size());
			for (const Durin::FMatrix4f& Matrix : Pose->Matrices)
				for (Durin::uint32 Column = 0; Column < 4; ++Column)
					for (Durin::uint32 Row = 0; Row < 4; ++Row)
						EXPECT_TRUE(std::isfinite(Matrix[Column][Row]));
			RuntimePoses.push_back(Pose);
			Component->UnregisterComponent();
			EXPECT_EQ(Component->GetLatestPosePalette(), nullptr);
		}
		ASSERT_EQ(RuntimePoses.size(), 4u);
		for (size_t MeshWithinContainer = 0; MeshWithinContainer < 2; ++MeshWithinContainer)
		{
			const auto& GltfPose = RuntimePoses[MeshWithinContainer];
			const auto& GlbPose = RuntimePoses[MeshWithinContainer + 2];
			ASSERT_EQ(GltfPose->Matrices.size(), GlbPose->Matrices.size());
			for (size_t MatrixIndex = 0; MatrixIndex < GltfPose->Matrices.size(); ++MatrixIndex)
				for (Durin::uint32 Column = 0; Column < 4; ++Column)
					for (Durin::uint32 Row = 0; Row < 4; ++Row)
						EXPECT_NEAR(
							GltfPose->Matrices[MatrixIndex][Column][Row],
							GlbPose->Matrices[MatrixIndex][Column][Row], 1.0e-5f);
		}
		ShutdownAssetManager();
	}
	InitializeAssetManager();
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::AuthoredEditor, {}}));
	Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousDerivedDataCache);
}
