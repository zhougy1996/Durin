#include "NativeAssetBuildTestSupport.h"
#include "MaterialRenderingTestFixture.h"
#include "Widgets/MMaterialEditor.h"
#include "Documents/EditorDocumentManager.h"
#include "MaterialAssetDefinitions.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"

namespace Durin::MaterialEditor
{
	struct FMaterialEditorTestAccess
	{
		static auto CreatePreview(MMaterialEditor& Editor, uint64 Id) -> void
		{
			Editor.MaterialPreviews.emplace(Id, std::make_shared<FMaterialPreview>(Id));
		}
	};
}

TEST(FMaterialPreviewTests, ClosingRetainedToolkitReleasesItsPreviewBeforeDestruction)
{
	InitializeDObjectSystem();
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	Durin::FModuleManager::Get().LoadModuleChecked("TextureCompressor");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	Durin::InitializeAssetCompilingManager();
	FMaterialPreviewHarness Harness;
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
	Durin::TStrongObjectPtr<Durin::DEditorEngine> Editor(Durin::NewObject<Durin::DEditorEngine>(nullptr, {}));
	Durin::FEditorEngineTestAccess::StartEditor(*Editor);
	auto* AssetEditorSubsystem = Editor->GetEditorSubsystem<Durin::DAssetEditorSubsystem>();
	ASSERT_NE(AssetEditorSubsystem, nullptr);
	auto* Instance = AssetEditorSubsystem->OpenAssetEditor(Durin::MaterialEditor::Z_Construct_DClass_DMaterialAssetEditor(), {
		.OpenArgs = {.AssetEditors = AssetEditorSubsystem}, .Assets = {Fixtures.Material}});
	ASSERT_NE(Instance, nullptr);
	const auto Toolkit = Instance->GetToolkit();
	constexpr uint64 PreviewId = 987654323;
	const auto LightName = std::format("MaterialPreviewLight_{}", PreviewId);
	Durin::MaterialEditor::FMaterialEditorTestAccess::CreatePreview(
		static_cast<Durin::MaterialEditor::MMaterialEditor&>(*Toolkit), PreviewId);
	ASSERT_NE(FindObjectByName(LightName), nullptr);
	Editor->PreExit();
	EXPECT_TRUE(Toolkit->IsClosed());
	EXPECT_TRUE(Toolkit->CloseWindow(Durin::EAssetEditorCloseReason::AssetEditorHostClosed));
	// Exercise renderer scene-count assertions before collecting the editor or World.
	// Both the toolkit and reflected editor still have strong references here.
	Durin::FlushRenderingCommands();
	Harness.ShutdownRenderer();
	Durin::MarkObjectHierarchyAsGarbage(Editor.Get());
	Editor.Reset();
	Durin::FlushRenderingCommands();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(FindObjectByName(LightName), nullptr);
	EXPECT_EQ(Durin::FAssetRetentionService::NumRetained(), 0u);
}

TEST(FMaterialPreviewTests, EngineMaterialPreviewMeshesAreSharedRetainedAssets)
{
	InitializeDObjectSystem();
	FScopedPreviewMeshCompiler MeshCompiler;
	ASSERT_TRUE(Durin::GetStaticMeshCompilationManagerDiagnostics().bAcceptingRequests);
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	InitializeDObjectSystem();
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	for (const std::string_view PathText : {
		"/Engine/Models/Sphere.Sphere",
		"/Engine/Models/Box.Box"})
	{
		Durin::FSoftObjectPath Path;
		ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(PathText, Path));
		Durin::FRetainedAsset First;
		Durin::FRetainedAsset Second;
		std::string Error;
		ASSERT_TRUE(Durin::FAssetRetentionService::Acquire(Path, First, Error)) << Error;
		ASSERT_TRUE(Durin::FAssetRetentionService::Acquire(Path, Second, Error)) << Error;
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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::FAssetRetentionService::NumRetained(), 0u);
}


TEST(FMaterialPreviewTests, MaterialPreviewDocumentsShareAssetsAcrossGarbageCollectionAndTeardown)
{
	InitializeDObjectSystem();
	FScopedPreviewMeshCompiler MeshCompiler;
	ASSERT_TRUE(Durin::GetStaticMeshCompilationManagerDiagnostics().bAcceptingRequests);
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	Durin::FModuleManager::Get().LoadModuleChecked("TextureCompressor");
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
		Durin::MaterialEditor::FMaterialPreview FirstPreview(FirstPreviewId);
		Durin::MaterialEditor::FMaterialPreview SecondPreview(SecondPreviewId);
		ASSERT_EQ(Durin::FAssetRetentionService::NumRetained(), 2u);
		ASSERT_NE(FindObjectByName(FirstLightName), nullptr);
		ASSERT_NE(FindObjectByName(SecondLightName), nullptr);

		Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
		Durin::FSoftObjectPath SpherePath;
		Durin::FSoftObjectPath BoxPath;
		ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate("/Engine/Models/Sphere.Sphere", SpherePath));
		ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate("/Engine/Models/Box.Box", BoxPath));
		Durin::FRetainedAsset SphereAsset;
		Durin::FRetainedAsset BoxAsset;
		std::string Error;
		ASSERT_TRUE(Durin::FAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)) << Error;
		ASSERT_TRUE(Durin::FAssetRetentionService::Acquire(BoxPath, BoxAsset, Error)) << Error;
		auto* Sphere = Durin::Cast<Durin::DStaticMesh>(SphereAsset.Get());
		auto* Box = Durin::Cast<Durin::DStaticMesh>(BoxAsset.Get());
		ASSERT_NE(Sphere, nullptr);
		ASSERT_NE(Box, nullptr);
		// Retention starts asynchronous preparation; a fresh process has no prepared mesh state.
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Sphere);
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Box);
		ASSERT_FALSE(Sphere->IsCompiling());
		ASSERT_FALSE(Box->IsCompiling());
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		ASSERT_NE(Box->GetRenderData(), nullptr);
		EXPECT_FALSE(Sphere->GetRenderData()->LODResources.empty());
		EXPECT_FALSE(Box->GetRenderData()->LODResources.empty());
	}

	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::FAssetRetentionService::NumRetained(), 0u);
	EXPECT_EQ(FindObjectByName(FirstLightName), nullptr);
	EXPECT_EQ(FindObjectByName(SecondLightName), nullptr);
}
