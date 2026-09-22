#include "MaterialRenderingTestFixture.h"

TEST(FMaterialPreviewTests, EngineMaterialPreviewMeshesAreSharedRetainedAssets)
{
	InitializeDObjectSystem();
	FScopedPreviewMeshCompiler MeshCompiler;
	ASSERT_TRUE(Durin::GetStaticMeshCompilationManagerDiagnostics().bAcceptingRequests);
	Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	InitializeDObjectSystem();
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	for (const std::string_view PathText : {
		"/Engine/Models/Sphere.Sphere",
		"/Engine/Models/Box.Box"})
	{
		Durin::FObjectPath Path;
		ASSERT_TRUE(Durin::FObjectPath::TryCreate(PathText, Path));
		Durin::Editor::FRetainedAsset First;
		Durin::Editor::FRetainedAsset Second;
		std::string Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(Path, First, Error)) << Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(Path, Second, Error)) << Error;
		EXPECT_EQ(First.Get(), Second.Get());
		auto* Mesh = Durin::Cast<Durin::DStaticMesh>(First.Get());
		ASSERT_NE(Mesh, nullptr) << Error;
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
		const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		ASSERT_NE(RenderData, nullptr);
		ASSERT_EQ(RenderData->LODResources.size(), 1u);
		const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		EXPECT_GT(LOD.GetNumVertices(), 8u);
		EXPECT_GT(LOD.GetNumIndices(), 12u);
		EXPECT_EQ(LOD.NumTexCoords, 1u);
		EXPECT_EQ(
			LOD.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer.GetTexCoords()[0].size(),
			LOD.GetNumVertices());
	}
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 0u);
}

TEST(FMaterialPreviewTests, EditorPreviewMeshOwnerRetainsPreparedMeshesWithoutOpenPreviews)
{
	InitializeDObjectSystem();
	FScopedPreviewMeshCompiler MeshCompiler;
	Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	Durin::Editor::FPreviewMeshResources Resources;
	std::string Error;
	ASSERT_TRUE(Resources.Initialize());
	auto* Sphere = Resources.GetSphere();
	auto* Box = Resources.GetBox();
	ASSERT_NE(Sphere, nullptr);
	ASSERT_NE(Box, nullptr);
	const auto* SphereData = Sphere->GetRenderData();
	const auto* BoxData = Box->GetRenderData();
	ASSERT_NE(SphereData, nullptr);
	ASSERT_NE(BoxData, nullptr);
	EXPECT_FALSE(Durin::HasPendingStaticMeshCompilation(*Sphere));
	EXPECT_FALSE(Durin::HasPendingStaticMeshCompilation(*Box));
	for (uint32 Visit = 0; Visit < 3; ++Visit)
	{
		Durin::FObjectPath Path;
		ASSERT_TRUE(Durin::FObjectPath::TryCreate(Resources.SphereAssetPath, Path));
		Durin::Editor::FRetainedAsset Consumer;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(Path, Consumer, Error)) << Error;
		EXPECT_EQ(Consumer.Get(), Sphere);
		Consumer = {};
		Durin::CollectGarbage();
		EXPECT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 2u);
		ASSERT_TRUE(Resources.Initialize());
		EXPECT_EQ(Resources.GetSphere(), Sphere);
		EXPECT_EQ(Resources.GetBox(), Box);
		EXPECT_EQ(Sphere->GetRenderData(), SphereData);
		EXPECT_EQ(Box->GetRenderData(), BoxData);
	}
	Resources.Reset();
	Resources.Reset();
	EXPECT_EQ(Resources.GetSphere(), nullptr);
	EXPECT_EQ(Resources.GetBox(), nullptr);
	EXPECT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 0u);
	Durin::CollectGarbage();
}

TEST(FMaterialPreviewTests, MaterialPreviewDocumentsShareAssetsAcrossGarbageCollectionAndTeardown)
{
	InitializeDObjectSystem();
	FScopedPreviewMeshCompiler MeshCompiler;
	ASSERT_TRUE(Durin::GetStaticMeshCompilationManagerDiagnostics().bAcceptingRequests);
	Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	FMaterialPreviewHarness Harness;
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());

	constexpr uint64 FirstPreviewId = 987654321;
	constexpr uint64 SecondPreviewId = 987654322;
	const std::string FirstLightName = std::format("MaterialPreviewLight_{}", FirstPreviewId);
	const std::string SecondLightName = std::format("MaterialPreviewLight_{}", SecondPreviewId);
	{
		Durin::Editor::Material::FMaterialPreview FirstPreview(FirstPreviewId);
		Durin::Editor::Material::FMaterialPreview SecondPreview(SecondPreviewId);
		ASSERT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 2u);
		ASSERT_NE(FindObjectByName(FirstLightName), nullptr);
		ASSERT_NE(FindObjectByName(SecondLightName), nullptr);

		Durin::CollectGarbage();
		Durin::FObjectPath SpherePath;
		Durin::FObjectPath BoxPath;
		ASSERT_TRUE(Durin::FObjectPath::TryCreate("/Engine/Models/Sphere.Sphere", SpherePath));
		ASSERT_TRUE(Durin::FObjectPath::TryCreate("/Engine/Models/Box.Box", BoxPath));
		Durin::Editor::FRetainedAsset SphereAsset;
		Durin::Editor::FRetainedAsset BoxAsset;
		std::string Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)) << Error;
		ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(BoxPath, BoxAsset, Error)) << Error;
		auto* Sphere = Durin::Cast<Durin::DStaticMesh>(SphereAsset.Get());
		auto* Box = Durin::Cast<Durin::DStaticMesh>(BoxAsset.Get());
		ASSERT_NE(Sphere, nullptr);
		ASSERT_NE(Box, nullptr);
		// Retention starts asynchronous preparation; a fresh process has no prepared mesh state.
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Sphere);
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Box);
		ASSERT_FALSE(Durin::HasPendingStaticMeshCompilation(*Sphere));
		ASSERT_FALSE(Durin::HasPendingStaticMeshCompilation(*Box));
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		ASSERT_NE(Box->GetRenderData(), nullptr);
		EXPECT_FALSE(Sphere->GetRenderData()->LODResources.empty());
		EXPECT_FALSE(Box->GetRenderData()->LODResources.empty());
	}

	Durin::CollectGarbage();
	EXPECT_EQ(Durin::Editor::FAssetRetentionService::NumRetained(), 0u);
	EXPECT_EQ(FindObjectByName(FirstLightName), nullptr);
	EXPECT_EQ(FindObjectByName(SecondLightName), nullptr);
}
