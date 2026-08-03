#include "MaterialTestSupport.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Texture/TextureCubeRenderResource.h"

TEST(FMaterialTests, StaticMeshProxyCapturesAssignedMaterialRenderData)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "ProxyMaterial");
	Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.25, 0.5, 0.75));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("MeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Snapshot = CaptureMaterialSlots(Harness.Scene);
	ASSERT_NE(Snapshot.Proxy, nullptr);
	ASSERT_EQ(Snapshot.Materials.size(), 1u);
	ExpectColorNear(Snapshot.Materials[0].BaseColor, Durin::FVector4f(0.25f, 0.5f, 0.75f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticPropertyChangesUpdatePipelineIdentityWithoutRecreatingProxy)
{
	FRenderSceneHarness Harness;
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "StaticIdentityMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Harness.CreateStaticMeshComponent("StaticIdentityComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Initial.Materials.size(), 1u);

	Durin::FMaterialStaticProperties Properties;
	Properties.BlendMode = Durin::EMaterialBlendMode::Masked;
	Properties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
	Properties.bTwoSided = true;
	Properties.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Disabled;
	Properties.OpacityMaskThreshold = 0.6f;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));

	const FMaterialSlotsSnapshot Updated = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Updated.Materials.size(), 1u);
	EXPECT_EQ(Updated.Proxy, Initial.Proxy);
	EXPECT_EQ(Updated.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(Updated.MaterialProxies, Initial.MaterialProxies);
	EXPECT_NE(
		Updated.Materials[0].PipelineIdentity,
		Initial.Materials[0].PipelineIdentity);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyCapturesPerSlotMaterials)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondSlotMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("MultiMaterialMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	Component->RegisterComponent();

	EXPECT_EQ(Component->GetNumMaterials(), 2u);
	EXPECT_EQ(Component->GetMaterial(), First);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	const FMaterialSlotsSnapshot Snapshot = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Snapshot.Materials.size(), 2u);
	ExpectColorNear(Snapshot.Materials[0].BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(Snapshot.Materials[1].BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyUsesEmptyFallbackForUnassignedSlots)
{
	FRenderSceneHarness Harness;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::FStaticMeshTestAccess::GetMutableRenderData(Mesh)
		->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("FallbackMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Snapshot = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Snapshot.Materials.size(), 2u);
	for (Durin::uint32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		const Durin::FMaterialRenderData& Fallback = Snapshot.Materials[SlotIndex];
		ExpectColorNear(Fallback.BaseColor, Durin::FMaterialRenderData{}.BaseColor);
		EXPECT_EQ(Fallback.BaseColorTexture, nullptr);
		EXPECT_FLOAT_EQ(Fallback.SpecularStrength, Durin::FMaterialRenderData{}.SpecularStrength);
		EXPECT_FLOAT_EQ(Fallback.Shininess, Durin::FMaterialRenderData{}.Shininess);
		EXPECT_EQ(Snapshot.MaterialProxies[SlotIndex], nullptr);
	}

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyResolvesPrecedenceAndUpdatesEverySharedMaterialSlot)
{
	FRenderSceneHarness Harness;
	auto* Shared = Durin::NewObject<Durin::DMaterial>(nullptr, "SharedSlotMaterial");
	auto* Override = Durin::NewObject<Durin::DMaterial>(nullptr, "OverrideSlotMaterial");
	Shared->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	Override->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.7, 0.6));
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 0))->DefaultMaterial = Shared;
	const Durin::FGuid OverrideId = AddDebugMaterialSlot(Mesh, "Override");
	const Durin::FGuid SharedId = AddDebugMaterialSlot(Mesh, "SharedAgain");
	AddDebugMaterialSlot(Mesh, "Fallback");
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 1))->DefaultMaterial = Shared;
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 2))->DefaultMaterial = Shared;
	auto* Component = Harness.CreateStaticMeshComponent("SharedSlotComponent");
	Component->SetStaticMesh(Mesh);
	ASSERT_TRUE(Component->SetMaterialBySlotId(OverrideId, Override));
	Component->RegisterComponent();

	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Initial.Materials.size(), 4u);
	ExpectColorNear(Initial.Materials[0].BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ExpectColorNear(Initial.Materials[1].BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));
	ExpectColorNear(Initial.Materials[2].BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ExpectColorNear(Initial.Materials[3].BaseColor, Durin::FMaterialRenderData{}.BaseColor);
	EXPECT_EQ(Component->GetMaterialBySlotId(SharedId), Shared);

	Shared->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.4, 0.5, 0.6));
	const FMaterialSlotsSnapshot Updated = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(Updated.Proxy, Initial.Proxy);
	EXPECT_EQ(Updated.ComponentRevision, Initial.ComponentRevision);
	EXPECT_EQ(Updated.MaterialProxies, Initial.MaterialProxies);
	ExpectColorNear(Updated.Materials[0].BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[1].BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[2].BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[3].BaseColor, Durin::FMaterialRenderData{}.BaseColor);
	EXPECT_EQ(
		Updated.Materials[0].PipelineIdentity,
		Initial.Materials[0].PipelineIdentity);
	EXPECT_EQ(Updated.MaterialProxies[0], Updated.MaterialProxies[2]);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Override);
	Durin::MarkAsGarbage(Shared);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyOrdersRapidBindingChangesAndRejectsStaleRevisions)
{
	FRenderSceneHarness Harness;
	auto* First = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidFirstMaterial");
	auto* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidSecondMaterial");
	auto* Replacement = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidReplacementMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
	auto* Component = Harness.CreateStaticMeshComponent("RapidSlotComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	const FMaterialSlotsSnapshot Rapid = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Rapid.Materials.size(), 2u);
	ExpectColorNear(Rapid.Materials[0].BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(Rapid.Materials[1].BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	EXPECT_EQ(Rapid.ComponentRevision, Initial.ComponentRevision);

	Replacement->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.9, 0.8, 0.7));
	Component->SetMaterial(0, Replacement);
	const FMaterialSlotsSnapshot Rebound = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(Rebound.Proxy, Initial.Proxy);
	EXPECT_GT(Rebound.ComponentRevision, Rapid.ComponentRevision);
	EXPECT_NE(Rebound.MaterialProxies[0], Rapid.MaterialProxies[0]);
	EXPECT_EQ(Rebound.MaterialProxies[1], Rapid.MaterialProxies[1]);
	ExpectColorNear(
		Rebound.Materials[0].BaseColor,
		Durin::FVector4f(0.9f, 0.8f, 0.7f, 1.0f));

	Durin::FMaterialRenderProxyBindingUpdate Stale;
	Stale.SlotIndex = 0;
	Stale.ComponentRevision = Rapid.ComponentRevision;
	Stale.MaterialProxy = First->GetMaterialRenderProxy();
	struct FApplyStaleMaterialBindingCommand
	{
		static constexpr const char* GetName() { return "ApplyStaleMaterialBinding"; }
	};
	Durin::EnqueueRenderCommand<FApplyStaleMaterialBindingCommand>(
		[Proxy = Rebound.Proxy, Stale](Durin::FRHICommandListImmediate&) {
			Proxy->UpdateMaterialRenderProxyBinding(Stale);
		});
	const FMaterialSlotsSnapshot Ordered = CaptureMaterialSlots(Harness.Scene);
	ExpectColorNear(Ordered.Materials[0].BaseColor, Rebound.Materials[0].BaseColor);
	EXPECT_EQ(Ordered.ComponentRevision, Rebound.ComponentRevision);
	EXPECT_EQ(Ordered.MaterialProxies[0], Rebound.MaterialProxies[0]);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Replacement);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, DebugStaticMeshProvidesCompleteSplitVertexAttributes)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const auto& Positions =
		LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
	const auto& TangentsVertexBuffer =
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer;
	EXPECT_EQ(Positions.size(), 3u);
	EXPECT_EQ(
		TangentsVertexBuffer.GetNormals().size(),
		Positions.size());
	EXPECT_EQ(
		TangentsVertexBuffer.GetTangents().size(),
		Positions.size());
	EXPECT_EQ(
		LOD.VertexBuffers.ColorVertexBuffer.GetColors().size(),
		Positions.size());
	for (const auto& Channel :
		LOD.VertexBuffers.StaticMeshVertexBuffer
			.TexCoordVertexBuffer.GetTexCoords())
	{
		EXPECT_EQ(Channel.size(), Positions.size());
	}
	ASSERT_EQ(LOD.Sections.size(), 1u);
	EXPECT_EQ(LOD.Sections[0].FirstIndex, 0u);
	EXPECT_EQ(LOD.Sections[0].IndexCount, 3u);

	const Durin::FStaticMeshPackedTangentBasis PackedTangentBasis =
		Durin::PackStaticMeshTangentBasis(
			Durin::FVector3f(0.0f, 0.0f, 1.0f),
			Durin::FVector4f(1.0f, 0.0f, 0.0f, -1.0f));
	const Durin::FStaticMeshColorVertex PackedColor =
		Durin::PackStaticMeshColor(
			Durin::FVector4f(1.0f, 0.5f, 0.0f, 0.25f));
	EXPECT_EQ(PackedTangentBasis.Normal[2], 32767);
	EXPECT_EQ(PackedTangentBasis.Tangent[0], 32767);
	EXPECT_EQ(PackedTangentBasis.Tangent[3], -32767);
	EXPECT_EQ(PackedColor.Color[0], 255);
	EXPECT_EQ(PackedColor.Color[1], 128);
	EXPECT_EQ(PackedColor.Color[2], 0);
	EXPECT_EQ(PackedColor.Color[3], 64);

	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, EngineMaterialPreviewMeshesAreSharedRetainedAssets)
{
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	for (const std::string_view PathText : {
		"/Engine/Editor/MaterialPreview/Sphere",
		"/Engine/Editor/MaterialPreview/Box"})
	{
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(PathText, Path));
		Durin::FRetainedEditorAsset First;
		Durin::FRetainedEditorAsset Second;
		std::string Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(Path, First, Error)) << Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(Path, Second, Error)) << Error;
		EXPECT_EQ(First.Get(), Second.Get());
		auto* Mesh = Durin::Cast<Durin::DStaticMesh>(First.Get());
		ASSERT_NE(Mesh, nullptr) << Error;
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
	EXPECT_EQ(Durin::FEditorAssetRetentionService::NumRetained(), 0u);
}

TEST(FMaterialTests, MaterialPreviewDocumentsShareAssetsAcrossGarbageCollectionAndTeardown)
{
	FMaterialPreviewHarness Harness;
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();

	constexpr Durin::uint64 FirstPreviewId = 987654321;
	constexpr Durin::uint64 SecondPreviewId = 987654322;
	const std::string FirstLightName = std::format("MaterialPreviewLight_{}", FirstPreviewId);
	const std::string SecondLightName = std::format("MaterialPreviewLight_{}", SecondPreviewId);
	{
		Durin::FMaterialPreview FirstPreview(FirstPreviewId);
		Durin::FMaterialPreview SecondPreview(SecondPreviewId);
		ASSERT_EQ(Durin::FEditorAssetRetentionService::NumRetained(), 2u);
		ASSERT_NE(FindObjectByName(FirstLightName), nullptr);
		ASSERT_NE(FindObjectByName(SecondLightName), nullptr);

		Durin::CollectGarbage();
		Durin::FAssetPath SpherePath;
		Durin::FAssetPath BoxPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Engine/Editor/MaterialPreview/Sphere", SpherePath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Engine/Editor/MaterialPreview/Box", BoxPath));
		Durin::FRetainedEditorAsset SphereAsset;
		Durin::FRetainedEditorAsset BoxAsset;
		std::string Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)) << Error;
		ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(BoxPath, BoxAsset, Error)) << Error;
		auto* Sphere = Durin::Cast<Durin::DStaticMesh>(SphereAsset.Get());
		auto* Box = Durin::Cast<Durin::DStaticMesh>(BoxAsset.Get());
		ASSERT_NE(Sphere, nullptr);
		ASSERT_NE(Box, nullptr);
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		ASSERT_NE(Box->GetRenderData(), nullptr);
		EXPECT_FALSE(Sphere->GetRenderData()->LODResources.empty());
		EXPECT_FALSE(Box->GetRenderData()->LODResources.empty());
	}

	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FEditorAssetRetentionService::NumRetained(), 0u);
	EXPECT_EQ(FindObjectByName(FirstLightName), nullptr);
	EXPECT_EQ(FindObjectByName(SecondLightName), nullptr);
}

TEST(FMaterialTests, RenderedThumbnailPreviewSceneCapturesResolvedMaterialDifferences)
{
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	const std::filesystem::path TextureMount =
		Durin::Testing::GetTestWorkDirectory() / "MaterialThumbnailVulkan";
	const std::filesystem::path TextureSource =
		Durin::Testing::GetTestWorkDirectory() / "MaterialThumbnailVulkan.png";
	Durin::Testing::RemoveTestWorkDirectory(TextureMount);
	std::filesystem::create_directories(TextureMount);
	WriteMaterialTextureFixture(TextureSource);
	std::vector<Durin::PathUtilities::FMountPoint> MountDefinitions(
		Durin::PathUtilities::GetRegisteredMountPoints().begin(),
		Durin::PathUtilities::GetRegisteredMountPoints().end());
	MountDefinitions.push_back({
		.VirtualRoot = "/MaterialThumbnailVulkan/",
		.Owner = Durin::PathUtilities::EMountOwner::Test,
		.Root = TextureMount,
		.bAssetPackages = true,
		.bAuthoringWritable = true});
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry(MountDefinitions);
	ASSERT_TRUE(MountRegistry.IsValid()) << MountRegistry.GetError();
	Durin::FAssetPath SpherePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::FRenderedAssetThumbnailVisualContract::SphereVirtualPath, SpherePath));
	Durin::FRetainedEditorAsset PreloadedSphere;
	std::string Error;
	ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(
		SpherePath, PreloadedSphere, Error)) << Error;
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	// A direct target run may leave deferred texture releases from earlier
	// CPU-only material cases. Drain them before creating the Vulkan device so
	// no stale game-thread owner survives into the RHI-backed portion.
	Durin::InitRenderingThread();
	Durin::CollectGarbage();
	WaitForRenderingThread();
	Durin::ShutdownRenderingThread();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	struct FBeginRenderedThumbnailFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginRenderedThumbnailFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginRenderedThumbnailFrame>(
		[](Durin::FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Durin::GDynamicRHI->RHIBeginFrame();
		});

	FMaterialTestEngine Engine;
	Durin::FRendererModule Renderer;
	Renderer.StartupModule();
	Engine.SetTestRendererModule(&Renderer);
	Durin::GEngine = &Engine;

	Durin::FRenderedAssetThumbnailVisualContract Contract;
	Contract.Output.Width = 64;
	Contract.Output.Height = 64;
	Durin::DStaticMesh* CaptureMesh = nullptr;
	Durin::DStaticMesh* CaptureSphere = nullptr;
	Durin::DMaterial* CaptureMaterial = nullptr;
	Durin::DMaterialInstance* CaptureInstance = nullptr;
	Durin::DTextureCube* CaptureCube = nullptr;
	Durin::FRHITextureReferenceRef CaptureCubeReference;
	Durin::FAssetPath CaptureTexturePath;
	Durin::FAssetPath CaptureCubePath;
	{
		Durin::FRenderedAssetThumbnailPreviewScenePool Pool(Contract);
		ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
		Durin::DStaticMesh* Sphere = Pool.GetSphereMesh();
		ASSERT_NE(Sphere, nullptr);
		CaptureSphere = Sphere;
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		CaptureMesh = Durin::DStaticMesh::CreateDebugTriangle();
		ASSERT_NE(CaptureMesh, nullptr);
		CaptureMaterial = Durin::NewObject<Durin::DMaterial>(
			nullptr, "RenderedThumbnailCaptureMaterial");
		CaptureInstance = Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, "RenderedThumbnailCaptureInstance");
		ASSERT_TRUE(CaptureMaterial->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.8, 0.15, 0.05)));
		ASSERT_TRUE(CaptureMaterial->SetScalarParameterValue(
			Durin::MaterialParameters::SpecularStrengthName(), 0.2f));
		ASSERT_TRUE(CaptureInstance->SetParent(CaptureMaterial));
		ASSERT_TRUE(CaptureInstance->SetVectorParameterValue(
			Durin::MaterialParameters::BaseColorName(),
			Durin::FVector3(0.05, 0.2, 0.8)));
		ASSERT_TRUE(CaptureInstance->SetScalarParameterValue(
			Durin::MaterialParameters::SpecularStrengthName(), 0.8f));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/T_Preview", CaptureTexturePath));
		const Durin::FTexture2DImportResult TextureResult =
			Durin::DTexture2D::ImportAsset(
				TextureSource.generic_string(), CaptureTexturePath.ToString());
		ASSERT_TRUE(TextureResult) << TextureResult.Message;
		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), TextureResult.Asset));
		Durin::FlushRenderingCommands();

		auto Capture = [&](Durin::DMaterialInterface* Material) {
			std::vector<Durin::uint8> Pixels;
			EXPECT_TRUE(Pool.SetMaterial(
				CaptureMesh, Material, Durin::FTransform(), Error)) << Error;
			EXPECT_TRUE(Pool.BeginCapture(Error, false)) << Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.PollCapture(Pixels, Error),
				Durin::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			Pool.Reset();
			return Pixels;
		};

		const std::vector<Durin::uint8> MaterialPixels =
			Capture(CaptureMaterial);
		const std::vector<Durin::uint8> InstancePixels =
			Capture(CaptureInstance);
		ASSERT_TRUE(CaptureMaterial->SetTextureParameterValue(
			Durin::MaterialParameters::BaseColorTextureName(), nullptr));
		const std::vector<Durin::uint8> UntexturedPixels =
			Capture(CaptureMaterial);
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MaterialThumbnailVulkan/TC_Preview", CaptureCubePath));
		const Durin::FTextureCubeImportResult CubeResult =
			Durin::DTextureCube::ImportAsset(
				Durin::Tests::GetRenderedThumbnailDirectionalCubeFaces(),
				CaptureCubePath.ToString());
		ASSERT_TRUE(CubeResult) << CubeResult.Message;
		CaptureCube = CubeResult.Asset;
		CaptureCubeReference = CaptureCube->GetTextureReferenceRHI();
		Durin::FlushRenderingCommands();
		std::vector<Durin::uint8> CubePixels;
		ASSERT_TRUE(Pool.SetTextureCube(
			CubeResult.Asset, Durin::FTransform(), Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
		Durin::FlushRenderingCommands();
		ASSERT_EQ(
			Pool.PollCapture(CubePixels, Error),
			Durin::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		Pool.Reset();
		ASSERT_EQ(MaterialPixels.size(), 64u * 64u * 4u);
		ASSERT_EQ(InstancePixels.size(), MaterialPixels.size());
		ASSERT_EQ(UntexturedPixels.size(), MaterialPixels.size());
		const size_t Corner = 0;
		const size_t Center = (32u * 64u + 32u) * 4u;
		EXPECT_EQ(MaterialPixels[Corner + 3], 0u);
		EXPECT_GT(MaterialPixels[Center + 3], 0u);
		EXPECT_EQ(CubePixels[Corner + 3], 255u);
		const std::array CornerRgb = {
			MaterialPixels[Corner], MaterialPixels[Corner + 1], MaterialPixels[Corner + 2]};
		const std::array MaterialCenterRgb = {
			MaterialPixels[Center], MaterialPixels[Center + 1], MaterialPixels[Center + 2]};
		const std::array InstanceCenterRgb = {
			InstancePixels[Center], InstancePixels[Center + 1], InstancePixels[Center + 2]};
		EXPECT_NE(CornerRgb, MaterialCenterRgb);
		EXPECT_NE(MaterialCenterRgb, InstanceCenterRgb);
		EXPECT_NE(MaterialPixels, UntexturedPixels);
		ASSERT_EQ(CubePixels.size(), MaterialPixels.size());
		const std::array CubeCenterRgb = {
			CubePixels[Center], CubePixels[Center + 1], CubePixels[Center + 2]};
		EXPECT_NE(CubeCenterRgb, CornerRgb);
		EXPECT_NE(CubeCenterRgb, (std::array<Durin::uint8, 3>{0, 0, 0}));
		std::unordered_set<Durin::uint32> CubeCornerColors;
		for (const size_t CornerPixel : std::array<size_t, 4>{
				0,
				(64u - 1u) * 4u,
				((64u - 1u) * 64u) * 4u,
				(64u * 64u - 1u) * 4u})
		{
			CubeCornerColors.insert(
				static_cast<Durin::uint32>(CubePixels[CornerPixel]) << 16
				| static_cast<Durin::uint32>(CubePixels[CornerPixel + 1]) << 8
				| CubePixels[CornerPixel + 2]);
		}
		EXPECT_GT(CubeCornerColors.size(), 1u);
		std::unordered_set<Durin::uint32> CubeColors;
		for (size_t Pixel = 0; Pixel < CubePixels.size(); Pixel += 4)
		{
			CubeColors.insert(
				static_cast<Durin::uint32>(CubePixels[Pixel]) << 16
				| static_cast<Durin::uint32>(CubePixels[Pixel + 1]) << 8
				| CubePixels[Pixel + 2]);
		}
		EXPECT_GT(CubeColors.size(), 8u);

		struct FEndRenderedThumbnailFrame
		{
			static constexpr auto GetName() -> const char* { return "EndRenderedThumbnailFrame"; }
		};
		Durin::EnqueueRenderCommand<FEndRenderedThumbnailFrame>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
	}

	Durin::GEngine = nullptr;
	ASSERT_NE(CaptureMesh, nullptr);
	ASSERT_NE(CaptureSphere, nullptr);
	ASSERT_TRUE(Durin::Asset::DeleteAsset(CaptureTexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(CaptureCubePath));
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(CaptureCube);
	Durin::MarkAsGarbage(CaptureInstance);
	Durin::MarkAsGarbage(CaptureMaterial);
	Durin::MarkAsGarbage(CaptureMesh);
	PreloadedSphere = {};
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SpherePath));
	Durin::CollectGarbage();
	struct FRetireRenderedThumbnailCubeResource
	{
		static constexpr auto GetName() -> const char*
		{
			return "RetireRenderedThumbnailCubeResource";
		}
	};
	Durin::EnqueueRenderCommand<FRetireRenderedThumbnailCubeResource>(
		[Reference = std::move(CaptureCubeReference)](
			Durin::FRHICommandListImmediate&) {});
	Durin::FlushRenderingCommands();
	Durin::CollectGarbage();
	Renderer.ShutdownModule();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	// The native suite may create another RHI in the same process; force the
	// process-wide immediate list to acquire that device's context next time.
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
}
