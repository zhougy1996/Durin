#include "TextureTestSupport.h"

#include "Animation/AnimationClip.h"
#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "Asset/CanonicalResave.h"
#include "Asset/Compatibility.h"
#include "Materials/MaterialInstance.h"
#include "RenderingThread.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
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
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { Durin::InitRenderingThread(); }
			~FRenderingThreadScope() { Durin::ShutdownRenderingThread(); }
		};

		std::unique_ptr<FRenderingThreadScope> RenderingThread;
		std::unique_ptr<Durin::PathUtilities::FScopedMountRegistryFixture> Mounts;
		std::string Source;
		Durin::FAssetPath DestinationDirectory;

		FSceneFixture() = default;
		FSceneFixture(FSceneFixture&& Other) noexcept
			: RenderingThread(std::move(Other.RenderingThread)),
			Mounts(std::move(Other.Mounts)), Source(std::move(Other.Source)),
			DestinationDirectory(std::move(Other.DestinationDirectory)) {}
	};

	auto InitializeFixture(std::string_view Name,
		std::string_view FixturePath = "StaticModelMaterials/RenderedOpaqueDataUri.gltf")
		-> FSceneFixture
	{
		InitializeDObjectSystem();
		auto RenderingThread =
			std::make_unique<FSceneFixture::FRenderingThreadScope>();
		std::string Error;
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory()
			/ "SceneAssetForge" / std::string(Name);
		Durin::Testing::RemoveTestWorkDirectory(Root);
		std::filesystem::create_directories(Root / "Engine/Content");
		std::filesystem::create_directories(Root / "Project/Content/Scenes");
		auto Mounts = std::make_unique<Durin::PathUtilities::FScopedMountRegistryFixture>(
			std::vector<Durin::PathUtilities::FMountPoint>{
				{.VirtualRoot = "/Engine/", .Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Engine/Content", .bAutoScan = true,
					.bContentWritable = true},
				{.VirtualRoot = "/SceneImportTests/", .Owner = Durin::PathUtilities::EMountOwner::Test,
					.Root = Root / "Project/Content", .bAutoScan = true,
					.bContentWritable = true, .Dependencies = {"/Engine/"}}});
		EXPECT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		EXPECT_NE(Durin::AssetForge::Builtins::EnsureImportedSurfaceMaterial(Error), nullptr) << Error;
		const std::filesystem::path Input = std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ FixturePath;
		const std::string Extension = Input.extension().generic_string();
		std::filesystem::copy_file(Input,
			Root / "Project/Content/Scenes" / (std::string(Name) + Extension),
			std::filesystem::copy_options::overwrite_existing);
		if (FixturePath == "Skeletal/ContractExternal.gltf")
			std::filesystem::copy_file(
				std::filesystem::path(DURIN_TEST_DATA_DIR) / "Skeletal/Contract.bin",
				Root / "Project/Content/Scenes/Contract.bin",
				std::filesystem::copy_options::overwrite_existing);
		FSceneFixture Fixture;
		Fixture.RenderingThread = std::move(RenderingThread);
		Fixture.Mounts = std::move(Mounts);
		Fixture.Source = (Root / "Project/Content/Scenes"
			/ (std::string(Name) + Extension)).generic_string();
		Fixture.DestinationDirectory = MakeAssetPath(std::format(
			"/SceneImportTests/SceneImport/{}", Name));
		return Fixture;
	}

	auto RunScene(const FSceneFixture& Fixture)
		-> Durin::AssetForge::Builtins::FSceneImportResult
	{
		Durin::AssetForge::Builtins::FSceneImportResult Result;
		EXPECT_TRUE(Durin::AssetForge::Builtins::ImportSceneAssets(
			Fixture.Source, Fixture.DestinationDirectory,
			Durin::FStaticMeshImportSettings::MakeDurin(), Result)) << Result.Message;
		return Result;
	}
}

TEST(FSceneImportTests, AssetForgePublishesHeterogeneousGraph)
{
	const FSceneFixture Fixture = InitializeFixture("Heterogeneous");
	const auto Imported = RunScene(Fixture);
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_EQ(Imported.Outputs.size(), 3u);
	for (const auto& Output : Imported.Outputs)
	{
		Durin::DObject* Object = nullptr;
		EXPECT_TRUE(Durin::Asset::LoadAsset(Output.AssetPath, Object));
		EXPECT_NE(Object, nullptr);
	}
}

TEST(FSceneImportTests, AssetForgePublishesSkeletalDependencyGraph)
{
	const FSceneFixture Fixture = InitializeFixture(
		"Skeletal", "Skeletal/ContractExternal.gltf");
	const auto Imported = RunScene(Fixture);
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_EQ(Imported.Outputs.size(), 11u);
	size_t Skeletons = 0;
	size_t SkeletalMeshes = 0;
	size_t Animations = 0;
	for (const auto& Output : Imported.Outputs)
	{
		Durin::DObject* Object = nullptr;
		ASSERT_TRUE(Durin::Asset::LoadAsset(Output.AssetPath, Object));
		Skeletons += Durin::Cast<Durin::DSkeleton>(Object) != nullptr;
		SkeletalMeshes += Durin::Cast<Durin::DSkeletalMesh>(Object) != nullptr;
		Animations += Durin::Cast<Durin::DAnimationClip>(Object) != nullptr;
	}
	EXPECT_EQ(Skeletons, 2u);
	EXPECT_EQ(SkeletalMeshes, 2u);
	EXPECT_EQ(Animations, 4u);
}

TEST(FSceneImportTests, DirectImportHonorsCancellationBeforePublication)
{
	const FSceneFixture Fixture = InitializeFixture("Scheduled");
	Durin::AssetForge::Builtins::FSceneImportResult Result;
	EXPECT_FALSE(Durin::AssetForge::Builtins::ImportSceneAssets(
		Fixture.Source, Fixture.DestinationDirectory,
		Durin::FStaticMeshImportSettings::MakeDurin(), Result, [] { return true; }));
	EXPECT_FALSE(Result);
	EXPECT_FALSE(Durin::Asset::FindAssetExact(
		MakeAssetPath("/SceneImportTests/SceneImport/Scheduled/StaticMeshes/Scheduled")));
}

TEST(FSceneImportTests, RuntimeOutputsDoNotReflectSceneOwnershipState)
{
	InitializeDObjectSystem();
	for (Durin::DClass* Class : {Durin::DStaticMesh::StaticClass(),
		Durin::DMaterialInstance::StaticClass(), Durin::DTexture2D::StaticClass(),
		Durin::DSkeleton::StaticClass(), Durin::DSkeletalMesh::StaticClass(),
		Durin::DAnimationClip::StaticClass()})
		EXPECT_EQ(Class->FindPropertyByName("ImportOwnership"), nullptr);
}
