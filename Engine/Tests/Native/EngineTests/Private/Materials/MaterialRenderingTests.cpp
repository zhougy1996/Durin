#include "MaterialTestSupport.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"

TEST(FMaterialTests, StaticMeshProxyCapturesAssignedMaterialRenderData)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "ProxyMaterial");
	Material->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.25, 0.5, 0.75));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "MeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.25f, 0.5f, 0.75f, 1.0f));

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyCapturesPerSlotMaterials)
{
	InitializeDObjectSystem();
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondSlotMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "MultiMaterialMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, First);
	Component->SetMaterial(1, Second);

	EXPECT_EQ(Component->GetNumMaterials(), 2u);
	EXPECT_EQ(Component->GetMaterial(), First);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	EXPECT_EQ(StaticMeshProxy->GetNumMaterials(), 2u);
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(0).BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));
	ExpectColorNear(StaticMeshProxy->GetMaterialRenderData(1).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyUsesEmptyFallbackForUnassignedSlots)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Mesh->GetRenderData()->MaterialSlots.push_back({"Second", 1});
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "FallbackMeshComponent");
	Component->SetStaticMesh(Mesh);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	auto* StaticMeshProxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(Proxy.get());
	ASSERT_NE(StaticMeshProxy, nullptr);
	EXPECT_EQ(StaticMeshProxy->GetNumMaterials(), 2u);
	for (Durin::uint32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		const Durin::FMaterialRenderData& Fallback = StaticMeshProxy->GetMaterialRenderData(SlotIndex);
		ExpectColorNear(Fallback.BaseColor, Durin::FMaterialRenderData{}.BaseColor);
		EXPECT_EQ(Fallback.BaseColorTexture, nullptr);
		EXPECT_FLOAT_EQ(Fallback.SpecularStrength, Durin::FMaterialRenderData{}.SpecularStrength);
		EXPECT_FLOAT_EQ(Fallback.Shininess, Durin::FMaterialRenderData{}.Shininess);
	}

	Proxy.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
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
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SharedSlotComponent");
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
	EXPECT_GT(Updated.ComponentRevision, Initial.ComponentRevision);
	ExpectColorNear(Updated.Materials[0].BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[1].BaseColor, Durin::FVector4f(0.8f, 0.7f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[2].BaseColor, Durin::FVector4f(0.4f, 0.5f, 0.6f, 1.0f));
	ExpectColorNear(Updated.Materials[3].BaseColor, Durin::FMaterialRenderData{}.BaseColor);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Override);
	Durin::MarkAsGarbage(Shared);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshProxyOrdersRapidCrossSlotUpdatesAndRejectsStaleRevisions)
{
	FRenderSceneHarness Harness;
	auto* First = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidFirstMaterial");
	auto* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "RapidSecondMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	AddDebugMaterialSlot(Mesh, "Second");
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "RapidSlotComponent");
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

	Durin::FMaterialRenderUpdate Newer;
	Newer.SlotIndex = 0;
	Newer.ComponentRevision = Rapid.ComponentRevision + 2;
	Newer.RenderData.BaseColor = Durin::FVector4f(0.9f, 0.8f, 0.7f, 1.0f);
	Durin::FMaterialRenderUpdate Stale = Newer;
	Stale.ComponentRevision = Rapid.ComponentRevision + 1;
	Stale.RenderData.BaseColor = Durin::FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
	struct FApplyOrderedMaterialUpdatesCommand
	{
		static constexpr const char* GetName() { return "ApplyOrderedMaterialUpdates"; }
	};
	Durin::EnqueueRenderCommand<FApplyOrderedMaterialUpdatesCommand>(
		[Proxy = Rapid.Proxy, Newer, Stale](Durin::FRHICommandListImmediate&) {
			Proxy->UpdateMaterialRenderData(Newer);
			Proxy->UpdateMaterialRenderData(Stale);
		});
	const FMaterialSlotsSnapshot Ordered = CaptureMaterialSlots(Harness.Scene);
	ExpectColorNear(Ordered.Materials[0].BaseColor, Newer.RenderData.BaseColor);
	EXPECT_EQ(Ordered.ComponentRevision, Newer.ComponentRevision);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, DebugStaticMeshProvidesCompleteLODAndPackedAttributes)
{
	InitializeDObjectSystem();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	ASSERT_EQ(RenderData->MaterialSlots.size(), 1u);
	const Durin::FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.Positions.size(), 3u);
	EXPECT_EQ(LOD.Normals.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Tangents.size(), LOD.Positions.size());
	EXPECT_EQ(LOD.Colors.size(), LOD.Positions.size());
	for (const auto& Channel : LOD.TexCoords) EXPECT_EQ(Channel.size(), LOD.Positions.size());
	ASSERT_EQ(LOD.Sections.size(), 1u);
	EXPECT_EQ(LOD.Sections[0].FirstIndex, 0u);
	EXPECT_EQ(LOD.Sections[0].IndexCount, 3u);

	std::array<Durin::FVector2f, Durin::MaxStaticMeshUVChannels> TexCoords{};
	const Durin::FStaticMeshPackedVertex Packed = Durin::PackStaticMeshVertex(
		Durin::FVector3f(0.0f, 0.0f, 1.0f), Durin::FVector4f(1.0f, 0.0f, 0.0f, -1.0f), TexCoords, Durin::FVector4f(1.0f, 0.5f, 0.0f, 0.25f));
	EXPECT_EQ(Packed.Normal[2], 32767);
	EXPECT_EQ(Packed.Tangent[0], 32767);
	EXPECT_EQ(Packed.Tangent[3], -32767);
	EXPECT_EQ(Packed.Color[0], 255);
	EXPECT_EQ(Packed.Color[1], 128);
	EXPECT_EQ(Packed.Color[2], 0);
	EXPECT_EQ(Packed.Color[3], 64);

	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, EngineMaterialPreviewMeshesAreSharedRetainedAssets)
{
	InitializeDObjectSystem();
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
		EXPECT_GT(LOD.Positions.size(), 8u);
		EXPECT_GT(LOD.Indices.size(), 12u);
		EXPECT_EQ(LOD.NumTexCoords, 1u);
		EXPECT_EQ(LOD.TexCoords[0].size(), LOD.Positions.size());
	}
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::FEditorAssetRetentionService::NumRetained(), 0u);
}

TEST(FMaterialTests, MaterialPreviewDocumentsShareAssetsAcrossGarbageCollectionAndTeardown)
{
	FMaterialPreviewHarness Harness;
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

TEST(FMaterialTests, RenderedThumbnailPreviewSceneRetainsSharedSphereAndCapturesOnce)
{
	InitializeDObjectSystem();
	Durin::PathUtilities::InitDefaultMountPoints();
	Durin::FAssetPath SpherePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::FRenderedAssetThumbnailVisualContract::SphereVirtualPath, SpherePath));
	Durin::FRetainedEditorAsset PreloadedSphere;
	std::string Error;
	ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(
		SpherePath, PreloadedSphere, Error)) << Error;
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
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
	{
		Durin::FRenderedAssetThumbnailPreviewScenePool Pool(Contract);
		ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
		Durin::DStaticMesh* Sphere = Pool.GetSphereMesh();
		ASSERT_NE(Sphere, nullptr);
		ASSERT_NE(Sphere->GetRenderData(), nullptr);
		CaptureMesh = Durin::DStaticMesh::CreateDebugTriangle();
		ASSERT_NE(CaptureMesh, nullptr);
		std::vector<Durin::FMaterialRenderUpdate> Materials;
		for (Durin::uint32 SlotIndex = 0;
			SlotIndex < CaptureMesh->GetRenderData()->MaterialSlots.size();
			++SlotIndex)
		{
			Materials.push_back({
				.SlotIndex = SlotIndex,
				.RenderData = Durin::FMaterialRenderData{
					.BaseColor = {0.8f, 0.15f, 0.05f, 1.0f}},
				.MaterialVersion = 1,
				.ComponentRevision = 1,
				.DirtyFlags = Durin::EMaterialRenderDirtyFlags::ParameterValues,
			});
		}
		auto Proxy = std::make_unique<Durin::FStaticMeshSceneProxy>(
			CaptureMesh->GetRenderData(), std::move(Materials));
		ASSERT_TRUE(Pool.SetPrimitive(std::move(Proxy), Durin::FMatrix(1.0), Error)) << Error;
		ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;

		struct FEndRenderedThumbnailFrame
		{
			static constexpr auto GetName() -> const char* { return "EndRenderedThumbnailFrame"; }
		};
		Durin::EnqueueRenderCommand<FEndRenderedThumbnailFrame>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();

		std::vector<Durin::uint8> Pixels;
		EXPECT_EQ(
			Pool.PollCapture(Pixels, Error),
			Durin::ERenderedAssetThumbnailCaptureState::Ready) << Error;
		ASSERT_EQ(Pixels.size(), 64u * 64u * 4u);
		const size_t Corner = 0;
		const size_t Center = (32u * 64u + 32u) * 4u;
		const std::array CornerRgb = {
			Pixels[Corner], Pixels[Corner + 1], Pixels[Corner + 2]};
		const std::array CenterRgb = {
			Pixels[Center], Pixels[Center + 1], Pixels[Center + 2]};
		EXPECT_NE(CornerRgb, CenterRgb);
		Pool.Reset();
	}

	Durin::GEngine = nullptr;
	ASSERT_NE(CaptureMesh, nullptr);
	CaptureMesh->GetRenderData()->ReleaseResources();
	Renderer.ReleaseResources();
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(CaptureMesh);
	PreloadedSphere = {};
	Durin::CollectGarbage();
	Renderer.ShutdownModule();
	Durin::ShutdownRenderingThread();
	// The native suite may create another RHI in the same process; force the
	// process-wide immediate list to acquire that device's context next time.
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
}
