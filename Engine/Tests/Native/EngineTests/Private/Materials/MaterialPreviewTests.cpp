#include "NativeAssetBuildTestSupport.h"
#include "MaterialRenderingTestFixture.h"
#include "FMaterialEditor.h"
#include "Widgets/MaterialEditingSession.h"
#include "MaterialAssetDefinitions.h"
#include "Materials/MaterialFunction.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Client/ViewportClient.h"
#include "Client/SceneViewport.h"
#include "Engine/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionSingleLayerWaterOutput.h"
#include "Misc/FileHelper.h"
#include "WaterRenderingTestSupport.h"

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
		Configuration.Properties.ShadingModel=EMaterialShadingModel::SingleLayerWater;
		ASSERT_TRUE(Material->SetMaterialConfiguration(Configuration));
		Frame();
		auto* Bed=Cast<DStaticMeshComponent>(FindObjectByName("WaterPreviewBed")); ASSERT_TRUE(Bed);
		ASSERT_TRUE(Bed->GetStaticMesh());
		EXPECT_EQ(Bed->GetMaterial(0),GetDefaultMaterial());
		EXPECT_EQ(Mesh->GetMaterial(0),Material.Get());
		Configuration.Properties.ShadingModel=EMaterialShadingModel::Unlit;
		ASSERT_TRUE(Material->SetMaterialConfiguration(Configuration));
		Frame();
		EXPECT_EQ(Bed->GetStaticMesh(),nullptr);
	}
	FViewportClient Owner; Owner.SetPostProcessMaterial(Material);
	auto Snapshot=Owner.GetViewSettings(); ASSERT_TRUE(Snapshot.PostProcess.Material);
	const FObjectKey Key(Material.Get()); Material.Reset(); CollectGarbage(EObjectFlags::Standalone);
	EXPECT_TRUE(Key.ResolveObjectPtr());
	Owner.SetPostProcessMaterial(nullptr); CollectGarbage(EObjectFlags::Standalone); FlushRenderingCommands();
	EXPECT_FALSE(Key.ResolveObjectPtr()); EXPECT_TRUE(Snapshot.PostProcess.Material);
	Snapshot.PostProcess.Material.reset(); FlushRenderingCommands();
}

TEST(FMaterialPreviewTests, WaterVulkanPreviewMatchesDirectRuntimeSubmission)
{
	using namespace Durin;
	using namespace Durin::Tests::Water;
	const char* Evidence = std::getenv("DURIN_WATER_PREVIEW_PROOF_DIR");
	if (!Evidence) GTEST_SKIP() << "Set DURIN_WATER_PREVIEW_PROOF_DIR for the Win64 Vulkan comparison.";
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModuleChecked("RenderCore");
	ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
	const std::shared_ptr<void> Shutdown(nullptr, [](void*) { RHIExit(); });
	Testing::FScopedMountRegistryFixture MountRegistry;
	FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FModuleManager::Get().LoadModuleChecked("TextureCompressor");
	FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	InitializeAssetCompilingManager();
	FMaterialPreviewHarness Harness;
	const std::shared_ptr<void> ClearAssets(nullptr, [](void*) {
		FlushRenderingCommands();
		CollectGarbage({});
		FlushRenderingCommands();
	});
	const std::shared_ptr<void> StopCompilation(nullptr, [](void*) { ShutdownAssetCompilingManager(); });
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialStaticProperties Properties;
	Properties.ShadingModel = EMaterialShadingModel::SingleLayerWater;
	Properties.bTwoSided = true;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	FMaterialRootInputs Inputs;
	Inputs.Get(EMaterialProperty::MP_Roughness).SetConstant({.1f});
	Inputs.Get(EMaterialProperty::MP_Opacity).SetConstant({.2f});
	TStrongObjectPtr<DMaterialExpressionSingleLayerWaterOutput> WaterOutput(
		NewObject<DMaterialExpressionSingleLayerWaterOutput>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionVector3Constant> Absorption(
		NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None));
	Absorption->Id = FGuid::NewGuid();
	Absorption->Value = {.002f, .001f, .0005f};
	WaterOutput->Id = FGuid::NewGuid();
	WaterOutput->AbsorptionCoefficients = {Absorption.Get()};
	const std::array<DMaterialExpression*, 2> Expressions{Absorption.Get(), WaterOutput.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Inputs));
	ASSERT_TRUE(Material->CompileEdits()) << FormatMaterialProgramDiagnostics(Material->GetMaterialCompileDiagnostics());
	MaterialEditor::FMaterialPreview Preview("WaterVulkanMatch");
	auto* World = Cast<DWorld>(FindObjectByName("MaterialPreview_WaterVulkanMatchWorld"));
	ASSERT_TRUE(World);
	FSoftObjectPath BoxPath;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/Engine/Models/Box.Box", BoxPath));
	FRetainedAsset Box;
	std::string Error;
	ASSERT_TRUE(FAssetRetentionService::Acquire(BoxPath, Box, Error)) << Error;
	FAssetCompilingManager::Get().FinishCompilationForObject(*Box.Get());
	Preview.SetMaterial(Material.Get());
	Preview.SetVisible(true);
	auto* Viewport = Harness.Engine.GetPreviewViewport();
	ASSERT_TRUE(Viewport);
	auto* Scene = Viewport->GetRenderScene();
	ASSERT_TRUE(Scene);
	Material->CacheResourceShadersForRendering(Scene->GetFeatureLevel(), EMaterialShaderPrecompileMode::Synchronous);
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram(Scene->GetFeatureLevel()))
		<< Material->GetMaterialCookDiagnostic(Scene->GetFeatureLevel());
	auto* PreviewActor = Cast<AActor>(FindObjectByName("MaterialPreviewActor_WaterVulkanMatch"));
	ASSERT_TRUE(PreviewActor);
	auto* PreviewMesh = PreviewActor->FindComponentByClass<DStaticMeshComponent>();
	ASSERT_TRUE(PreviewMesh && PreviewMesh->IsRegistered());
	ASSERT_EQ(PreviewMesh->GetMaterial(0), Material.Get());
	FSceneView View;
	ASSERT_TRUE(Harness.Engine.BuildSceneView(Viewport, 128, 128, false, View));
	View.ViewStateId = {};
	Viewport->RequestResourcePreparation();
	const auto TargetDesc = FRHITextureCreateDesc::Create2D("WaterPreviewComparison", 128, 128, EPixelFormat::SRGBA8_UNORM)
		.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
	FByteBuffer PreviewPixels, RuntimePixels;
	FRDGCapture Graph;
	bool bCaptured = false;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	do
	{
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		PumpGameThreadDeferredWork();
		Preview.SetVisible(true);
		World->Tick({.DeltaSeconds = 0.f});
		EnqueueRenderCommand("CaptureWaterPreview", [&](FRHICommandListImmediate& Commands) {
			FGeometryQualificationFrame Frame(Commands);
			Harness.RendererModule.BeginFrame_RenderThread(Commands);
			const auto Target = GDynamicRHI->RHICreateTexture(Commands, TargetDesc);
			ASSERT_TRUE(Target);
			const FSceneViewRenderRequest Request{Scene, View, Target, false, {}, nullptr, &Graph};
			const auto Result = Viewport->RenderView_RenderThread(Harness.RendererModule, Commands, Request);
			if (Result == ERenderViewResult::RendererResourcesPending) return;
			ASSERT_EQ(Result, ERenderViewResult::Success);
			if (std::ranges::count(Graph.Passes, "Water.SkyReflection", &FRDGPassCapture::Name) != 1) return;
			ReadWaterPixels(Commands, Target, PreviewPixels);
			ASSERT_EQ(Harness.RendererModule.RenderView(Commands, Request), ERenderViewResult::Success);
			ReadWaterPixels(Commands, Target, RuntimePixels);
			bCaptured = true;
		});
		FlushRenderingCommands();
		if (bCaptured || HasFatalFailure()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now() < Deadline);
	std::string PassNames;
	for (const auto& Pass : Graph.Passes) PassNames += Pass.Name + " ";
	ASSERT_TRUE(bCaptured) << PassNames << PreviewMesh->GetPSOPrecacheDiagnostic();
	ASSERT_EQ(PreviewPixels.size(), 128u * 128u * 4u);
	EXPECT_EQ(PreviewPixels, RuntimePixels);
	const std::filesystem::path Directory(Evidence);
	std::filesystem::create_directories(Directory);
	EXPECT_TRUE(FFileHelper::SaveArrayToFile(PreviewPixels, Directory / "preview.rgba8"));
	EXPECT_TRUE(FFileHelper::SaveArrayToFile(RuntimePixels, Directory / "direct-runtime.rgba8"));
}
