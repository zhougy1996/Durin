#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "Components/SkeletalMeshComponent.h"
#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "Asset/CanonicalResave.h"
#include "Asset/Compatibility.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "RenderingThread.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "SkeletalMesh/Skeleton.h"
#include "Thumbnail/SkeletalMeshThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"

namespace
{
	class FRenderingThreadScope final
	{
	public:
		FRenderingThreadScope() { Durin::InitRenderingThread(); }
		~FRenderingThreadScope() { Durin::ShutdownRenderingThread(); }
	};

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

	auto InitializeAssetManager(const std::filesystem::path& CookRoot = {}) -> void
	{
		if (CookRoot.empty())
		{
			ASSERT_TRUE(Durin::Asset::InitializeAssetManager());
			return;
		}
		auto Configuration = Durin::Asset::FAssetRuntimeConfiguration::Authored();
		ASSERT_TRUE(Durin::Asset::FAssetRuntimeConfiguration::Cooked(
			CookRoot, Configuration));
		ASSERT_TRUE(Durin::Asset::InitializeAssetManager(std::move(Configuration)));
	}

	using FCookTree = std::vector<std::pair<std::string, std::vector<std::byte>>>;

	auto ReadCookTree(const std::filesystem::path& Root, FCookTree& Out) -> bool
	{
		for (const std::filesystem::directory_entry& Entry :
			std::filesystem::recursive_directory_iterator(Root))
		{
			if (!Entry.is_regular_file()) continue;
			std::vector<std::byte> Bytes;
			if (!Durin::FFileHelper::LoadFileToArray(
				Bytes, Entry.path())) return false;
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
		std::vector<std::byte> Bytes;
		const Durin::Asset::FAssetResult Serialized =
			Durin::Asset::SerializeAssetPackageBytes(Object.GetPackage(), Bytes);
		if (!Serialized)
		{
			OutError = Serialized.Message;
			return false;
		}
		return Context.AddPackage(
			Object.GetPackage()->GetPackagePath(), std::move(Bytes), &OutError);
	}

	struct FSceneOutputs
	{
		std::vector<Durin::DSkeleton*> Skeletons;
		std::vector<Durin::DSkeletalMesh*> SkeletalMeshes;
		std::vector<Durin::DAnimationClip*> AnimationClips;
		std::vector<Durin::DMaterialInstance*> Materials;
	};

	auto LoadSceneOutputs(const Durin::AssetForge::Builtins::FSceneImportResult& Result)
		-> FSceneOutputs
	{
		FSceneOutputs Outputs;
		for (const auto& Mapping : Result.Outputs)
		{
			Durin::DObject* Object = nullptr;
			if (!Durin::Asset::LoadAsset(Mapping.AssetPath, Object) || !Object) continue;
			if (auto* Value = Durin::Cast<Durin::DSkeleton>(Object)) Outputs.Skeletons.push_back(Value);
			else if (auto* Value = Durin::Cast<Durin::DSkeletalMesh>(Object)) Outputs.SkeletalMeshes.push_back(Value);
			else if (auto* Value = Durin::Cast<Durin::DAnimationClip>(Object)) Outputs.AnimationClips.push_back(Value);
			else if (auto* Value = Durin::Cast<Durin::DMaterialInstance>(Object)) Outputs.Materials.push_back(Value);
		}
		return Outputs;
	}

	auto ExecuteSceneImport(std::string_view Source,
		const Durin::FAssetPath& Destination)
		-> FSceneOutputs
	{
		Durin::AssetForge::Builtins::FSceneImportResult Result;
		EXPECT_TRUE(Durin::AssetForge::Builtins::ImportSceneAssets(
			Source, Destination, Durin::FStaticMeshImportSettings::MakeDurin(),
			Result)) << Result.Message;
		return LoadSceneOutputs(Result);
	}

}

TEST(FSkeletalSceneLifecycleTests, GltfAndGlbCookDeterministicallyAndLoadRuntimeOnly)
{
	InitializeDObjectSystem();
	FRenderingThreadScope RenderingThread;
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
	{
		const std::array<Durin::PathUtilities::FMountPoint, 2> MountDefinitions{{
			{
				.VirtualRoot = "/Engine/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = EngineContent,
				.bAutoScan = true,
				.bContentWritable = true},
			{
				.VirtualRoot = "/Game/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = GameContent,
				.bAutoScan = true,
				.bContentWritable = true,
				.Dependencies = {"/Engine/"}}}};
		Durin::PathUtilities::FScopedMountRegistryFixture Mounts(MountDefinitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		std::string Error;
		Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
		Durin::DMaterial* StandardMaterial =
			Durin::AssetForge::Builtins::EnsureImportedSurfaceMaterial(Error);
		ASSERT_NE(StandardMaterial, nullptr) << Error;

		const std::array<std::pair<std::string_view, std::string_view>, 2> Cases{{
			{"DataUri", "Contract.gltf"},
			{"Binary", "Contract.glb"}}};
		std::array<FSceneOutputs, 2> Results;
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
			Results[CaseIndex] = ExecuteSceneImport(
				(GameContent / "Scenes" / (std::string(Name) + Extension)).generic_string(),
				MakeAssetPath(std::format("/Game/Imports/{}", Name)));
			const FSceneOutputs& Initial = Results[CaseIndex];
			ASSERT_EQ(Initial.Skeletons.size(), 2u);
			ASSERT_EQ(Initial.SkeletalMeshes.size(), 2u);
			ASSERT_EQ(Initial.AnimationClips.size(), 4u);
			ASSERT_EQ(Initial.Materials.size(), 2u);
			for (Durin::DSkeletalMesh* Mesh : Initial.SkeletalMeshes)
			{
				const Durin::FAssetPath MeshPath =
					MakeAssetPath(Mesh->GetPackage()->GetPackagePath());
				const Durin::Asset::FAssetCatalogEntry MeshData =
					Durin::Asset::FindAssetExact(MeshPath);
				ASSERT_NE(MeshData, nullptr);
				Durin::Editor::SkeletalMesh::DSkeletalMeshThumbnailRenderer Renderer;
				Durin::Editor::FAssetThumbnailGenerationRequest ThumbnailRequest;
				const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint{
					.VirtualPath = MeshData->PackagePath,
					.AssetClassName = MeshData->AssetClassName,
					.PackageFormatVersion = MeshData->FormatVersion,
					.FileSize = static_cast<uint64>(MeshData->FileSize),
					.LastWriteTimeTicks = MeshData->LastWriteTimeTicks};
				ASSERT_TRUE(Renderer.CaptureGenerationRequest({
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
					Durin::Editor::SkeletalMesh::FSkeletalMeshThumbnailGenerationInput>(
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

		}

		auto Cook = [&](const std::filesystem::path& CookRoot) -> bool {
			Durin::Asset::FCookContext Context(
				CookRoot, Durin::Asset::ECookTargetPlatform::Win64,
				Durin::Asset::ECookTargetProfile::Game);
			if (!Durin::Asset::ContributeEngineCookAsset(
				*StandardMaterial, StandardMaterial->GetPackage()->GetPackagePath(),
				Context, Error))
				return false;
			for (const FSceneOutputs& Result : Results)
			{
				for (Durin::DMaterialInstance* Material : Result.Materials)
					if (!AddPackageOnly(Context, *Material, Error)) return false;
				for (Durin::DSkeleton* Skeleton : Result.Skeletons)
					if (!Durin::Asset::ContributeEngineCookAsset(
						*Skeleton, Skeleton->GetPackage()->GetPackagePath(),
						Context, Error)) return false;
				for (Durin::DSkeletalMesh* Mesh : Result.SkeletalMeshes)
					if (!Durin::Asset::ContributeEngineCookAsset(
						*Mesh, Mesh->GetPackage()->GetPackagePath(), Context, Error))
						return false;
				for (Durin::DAnimationClip* Clip : Result.AnimationClips)
					if (!Durin::Asset::ContributeEngineCookAsset(
						*Clip, Clip->GetPackage()->GetPackagePath(), Context, Error))
						return false;
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
		for (const FSceneOutputs& Result : Results)
		{
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

	Durin::Testing::RemoveTestWorkDirectory(EngineContent);
	Durin::Testing::RemoveTestWorkDirectory(GameContent);
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	EXPECT_FALSE(std::filesystem::exists(GameContent / "Scenes/DataUri.gltf"));
	EXPECT_FALSE(std::filesystem::exists(GameContent / "Scenes/Binary.glb"));
	EXPECT_FALSE(std::filesystem::exists(CacheRoot));
	InitializeAssetManager(FirstCookRoot);
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
		ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
		std::vector<Durin::DSkeletalMesh*> RuntimeMeshes;
		RuntimeMeshes.reserve(MeshPaths.size());
		for (size_t Index = 0; Index < MeshPaths.size(); ++Index)
		{
			Durin::DSkeletalMesh* Mesh = nullptr;
			const Durin::Asset::FAssetResult LoadMeshResult =
				Durin::Asset::LoadAsset(MeshPaths[Index], Mesh);
			ASSERT_TRUE(LoadMeshResult) << LoadMeshResult.Message;
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
				for (uint32 Column = 0; Column < 4; ++Column)
					for (uint32 Row = 0; Row < 4; ++Row)
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
				for (uint32 Column = 0; Column < 4; ++Column)
					for (uint32 Row = 0; Row < 4; ++Row)
						EXPECT_NEAR(
							GltfPose->Matrices[MatrixIndex][Column][Row],
							GlbPose->Matrices[MatrixIndex][Column][Row], 1.0e-5f);
		}
		ShutdownAssetManager();
	}
	InitializeAssetManager();
	Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousDerivedDataCache);
}
