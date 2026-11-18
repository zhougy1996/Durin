#include "NativeAssetBuildTestSupport.h"
#include "MaterialRenderingTestFixture.h"
#include "FMaterialEditor.h"
#include "Widgets/MaterialEditingSession.h"
#include "Documents/EditorDocumentManager.h"
#include "MaterialAssetDefinitions.h"
#include "Materials/MaterialFunction.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Client/ViewportClient.h"
#include "Engine/Actor.h"
#include "Components/StaticMeshComponent.h"

namespace Durin::MaterialEditor
{
	struct FMaterialEditorTestAccess
	{
		static auto CreatePreview(FMaterialEditor& Editor) -> void
		{
			Editor.EditingSession->Preview = std::make_shared<FMaterialPreview>(Editor.GetWindowId());
		}
	};
}

TEST(FMaterialPreviewTests, ClosingRetainedToolkitReleasesMaterialAndFunctionPreviewsBeforeDestruction)
{
	InitializeDObjectSystem();
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	Durin::FModuleManager::Get().LoadModuleChecked("TextureCompressor");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests()) Durin::InitializeAssetCompilingManager();
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
	const auto LightName = std::format("MaterialPreviewLight_{}", Toolkit->GetWindowId());
	Durin::MaterialEditor::FMaterialEditorTestAccess::CreatePreview(
		static_cast<Durin::MaterialEditor::FMaterialEditor&>(*Toolkit));
	ASSERT_NE(FindObjectByName(LightName), nullptr);
	auto* Function = Durin::NewObject<Durin::DMaterialFunction>(Fixtures.Material->GetPackage(), "RetainedFunction");
	auto* FunctionInstance = AssetEditorSubsystem->OpenAssetEditor(
		Durin::MaterialEditor::Z_Construct_DClass_DMaterialFunctionAssetEditor(), {
			.OpenArgs = {.AssetEditors = AssetEditorSubsystem}, .Assets = {Function}});
	ASSERT_NE(FunctionInstance, nullptr);
	const auto FunctionToolkit = FunctionInstance->GetToolkit();
	const auto FunctionLightName = std::format("MaterialPreviewLight_{}",
		FunctionToolkit->GetWindowId());
	ASSERT_NE(FindObjectByName(FunctionLightName), nullptr);
	Editor->PreExit();
	EXPECT_TRUE(FunctionToolkit->IsClosed());
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
	EXPECT_EQ(FindObjectByName(FunctionLightName), nullptr);
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
		Durin::MaterialEditor::FMaterialPreview FirstPreview(std::to_string(FirstPreviewId));
		Durin::MaterialEditor::FMaterialPreview SecondPreview(std::to_string(SecondPreviewId));
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

TEST(FMaterialPreviewTests, PostProcessUsesStudioInputAndDomainSwitchRebindsTheSameMaterial)
{
	using namespace Durin;
	InitializeDObjectSystem(); FScopedPreviewMeshCompiler Compiler;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	Testing::FScopedMountRegistryFixture MountRegistry;
	FMountPaths::InitDefaultMountPoints(); ASSERT_TRUE(RefreshAssetRegistry());
	FMaterialPreviewHarness Harness;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr,NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialConfiguration Configuration; Configuration.Properties.ShadingModel=EMaterialShadingModel::Unlit;
	ASSERT_TRUE(Material->SetMaterialConfiguration(Configuration));

	{
		MaterialEditor::FMaterialPreview Preview("987654324");
		for (const auto Text : {"/Engine/Models/Sphere.Sphere","/Engine/Models/Box.Box"})
		{
			FSoftObjectPath Path; ASSERT_TRUE(FSoftObjectPath::TryCreate(Text,Path));
			FRetainedAsset Asset; std::string Error; ASSERT_TRUE(FAssetRetentionService::Acquire(Path,Asset,Error)) << Error;
			FAssetCompilingManager::Get().FinishCompilationForObject(*Cast<DStaticMesh>(Asset.Get()));
		}
		auto* Actor=Cast<AActor>(FindObjectByName("MaterialPreviewActor_987654324")); ASSERT_TRUE(Actor);
		auto* Mesh=Actor->FindComponentByClass<DStaticMeshComponent>(); ASSERT_TRUE(Mesh);
		const auto Frame=[&] { Preview.SetMaterial(Material); FlushRenderingCommands(); };
		Frame(); Frame(); ASSERT_GT(Mesh->GetNumMaterials(),0u); EXPECT_EQ(Mesh->GetMaterial(0),Material.Get());
		Configuration.Domain=EMaterialDomain::PostProcess; ASSERT_TRUE(Material->SetMaterialConfiguration(Configuration));
		Frame(); EXPECT_EQ(Mesh->GetMaterial(0),GetDefaultMaterial());
		Configuration.Domain=EMaterialDomain::Surface; ASSERT_TRUE(Material->SetMaterialConfiguration(Configuration));
		Frame(); EXPECT_EQ(Mesh->GetMaterial(0),Material.Get());
	}
	FViewportClient Owner; Owner.SetPostProcessMaterial(Material);
	auto Snapshot=Owner.GetViewSettings(); ASSERT_TRUE(Snapshot.PostProcess.Material);
	const FObjectKey Key(Material.Get()); Material.Reset(); CollectGarbage(EObjectFlags::Standalone);
	EXPECT_TRUE(Key.ResolveObjectPtr());
	Owner.SetPostProcessMaterial(nullptr); CollectGarbage(EObjectFlags::Standalone); FlushRenderingCommands();
	EXPECT_FALSE(Key.ResolveObjectPtr()); EXPECT_TRUE(Snapshot.PostProcess.Material);
	Snapshot.PostProcess.Material.reset(); FlushRenderingCommands();
}
