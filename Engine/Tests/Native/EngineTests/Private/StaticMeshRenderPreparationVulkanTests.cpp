#include "Asset/PackageSerialization.h"
#include "VulkanEngineTestSupport.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Asset/Load.h"
#include "Asset/AssetCook.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "Math/Operations.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "SceneTestAccess.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

namespace
{
	auto MakeMaterial(Durin::EMaterialBlendMode BlendMode, bool bTwoSided = false, Durin::EMaterialDepthWritePolicy DepthWrite = Durin::EMaterialDepthWritePolicy::Automatic)
		-> Durin::FMaterialRenderProxyRef
	{
		const char* Name = BlendMode == Durin::EMaterialBlendMode::Masked
			? "PreparationMaskedMaterial"
			: (BlendMode == Durin::EMaterialBlendMode::Translucent
				? "PreparationTranslucentMaterial"
				: (bTwoSided ? "PreparationTwoSidedMaterial"
					: "PreparationOpaqueMaterial"));
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		EXPECT_TRUE(Material->SetStaticProperties(Durin::FMaterialStaticProperties{
			.BlendMode = BlendMode,
			.ShadingModel = Durin::EMaterialShadingModel::Lit,
			.bTwoSided = bTwoSided,
			.DepthWritePolicy = DepthWrite,
			.OpacityMaskThreshold = 0.4f
		}));
		return Material->GetMaterialRenderProxy();
	}

	auto MakeRenderData() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Result = std::make_unique<Durin::FStaticMeshRenderData>();
		Result->MaterialSlots.resize(4);
		Result->LODResources.resize(1);
		auto& LOD = Result->LODResources[0];
		const std::vector<Durin::FVector3f> Positions{
			{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}
		};
		LOD.VertexBuffers.PositionVertexBuffer.Init(Positions);
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(3, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(3, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels> TexCoords;
		for (auto& Channel : TexCoords)
		{
			Channel.resize(3);
		}
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(TexCoords), 3, 1
		);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(3, Durin::FVector4f(1.0f)), 3
		);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2});
		for (uint32 SectionIndex = 0; SectionIndex < 4; ++SectionIndex)
		{
			LOD.Sections.push_back({.Name = std::format("Section{}", SectionIndex), .FirstIndex = SectionIndex * 3, .IndexCount = 3, .MinVertexIndex = 0, .MaxVertexIndex = 2, .MaterialSlotIndex = SectionIndex, .LocalBounds = Durin::FBox(Durin::FVector3(-1.0, -1.0, static_cast<double>(SectionIndex)), Durin::FVector3(1.0, 1.0, static_cast<double>(SectionIndex)))});
		}
		LOD.NumTexCoords = 1;
		LOD.bHasColorVertexData = true;
		Result->LODVertexFactories.resize(1);
		Result->RecalculateBounds();
		return Result;
	}

	auto MakeMultiLODRenderData()
		-> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Result = std::make_unique<Durin::FStaticMeshRenderData>();
		Result->MaterialSlots.resize(1);
		Result->LODResources.resize(3);
		const std::array<std::vector<Durin::FVector3f>, 3> Positions{{
			{{5.0f, -1.0f, -1.0f}, {5.0f, 1.0f, -1.0f},
			 {5.0f, 1.0f, 1.0f}, {5.0f, -1.0f, 1.0f},
			 {5.0f, 0.0f, 0.0f}},
			{{5.0f, -1.0f, -1.0f}, {5.0f, 1.0f, -1.0f},
			 {5.0f, 1.0f, 1.0f}, {5.0f, -1.0f, 1.0f}},
			{{5.0f, -1.0f, -1.0f}, {5.0f, 1.0f, -1.0f},
			 {5.0f, 0.0f, 1.0f}}}};
		const std::array<std::vector<uint32>, 3> Indices{{
			{0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4},
			{0, 1, 2, 0, 2, 3},
			{0, 1, 2}}};
		const std::array<float, 3> ScreenSizes{0.5f, 0.25f, 0.0f};
		for (size_t LODIndex = 0; LODIndex < Result->LODResources.size();
			 ++LODIndex)
		{
			auto& LOD = Result->LODResources[LODIndex];
			LOD.VertexBuffers.PositionVertexBuffer.Init(Positions[LODIndex]);
			LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
				std::vector<Durin::FVector3f>(
					Positions[LODIndex].size(), {1.0f, 0.0f, 0.0f}),
				std::vector<Durin::FVector4f>(
					Positions[LODIndex].size(), {0.0f, 1.0f, 0.0f, 1.0f}));
			std::array<
				std::vector<Durin::FVector2f>,
				Durin::MaxStaticMeshUVChannels> TexCoords;
			for (auto& Channel : TexCoords)
			{
				Channel.resize(Positions[LODIndex].size());
			}
			LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
				std::move(TexCoords),
				static_cast<uint32>(Positions[LODIndex].size()), 1);
			LOD.VertexBuffers.ColorVertexBuffer.Init(
				std::vector<Durin::FVector4f>(
					Positions[LODIndex].size(), Durin::FVector4f(1.0f)),
				static_cast<uint32>(Positions[LODIndex].size()));
			LOD.IndexBuffer.Init(Indices[LODIndex]);
			LOD.Sections.push_back({
				.Name = std::format("LOD{}", LODIndex),
				.FirstIndex = 0,
				.IndexCount = static_cast<uint32>(Indices[LODIndex].size()),
				.MinVertexIndex = 0,
				.MaxVertexIndex = static_cast<uint32>(
					Positions[LODIndex].size() - 1),
				.MaterialSlotIndex = 0,
				.LocalBounds = Durin::FBox(
					{5.0, -1.0, -1.0}, {5.0, 1.0, 1.0})});
			LOD.ScreenSize = ScreenSizes[LODIndex];
			LOD.NumTexCoords = 1;
			LOD.bHasColorVertexData = true;
		}
		Result->LODVertexFactories.resize(3);
		Result->RecalculateBounds();
		return Result;
	}

	struct FPreparationSummary
	{
		size_t Opaque = 0;
		size_t Masked = 0;
		size_t Translucent = 0;
		Durin::ERHIFrontFace MirroredFrontFace = Durin::ERHIFrontFace::Clockwise;
		bool bTranslucentDepthWrite = true;
		bool bTranslucentBlend = false;
		double FirstViewDistance = 0.0;
		double SecondViewDistance = 0.0;
	};

	struct FInitializePreparedStaticMeshResourcesCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "InitializePreparedStaticMeshResources";
		}
	};
	struct FCapturePreparedStaticMeshViewCommand
	{
		static constexpr auto GetName() -> const char* { return "CapturePreparedStaticMeshView"; }
	};

	class FScopedRenderThreadBlocker final
	{
	public:
		FScopedRenderThreadBlocker()
			: State(std::make_shared<FState>())
		{
			Durin::EnqueueRenderCommand<FBlockCookedStaticMeshInitializationCommand>(
				[State = State](Durin::FRHICommandListImmediate&) {
					std::unique_lock Lock(State->Mutex);
					State->bEntered = true;
					State->CV.notify_all();
					State->CV.wait(Lock, [&] { return State->bReleased; });
				});
			std::unique_lock Lock(State->Mutex);
			State->CV.wait(Lock, [&] { return State->bEntered; });
		}

		~FScopedRenderThreadBlocker()
		{
			Release();
		}

		auto Release() -> void
		{
			std::lock_guard Lock(State->Mutex);
			State->bReleased = true;
			State->CV.notify_all();
		}

	private:
		struct FState
		{
			std::mutex Mutex;
			std::condition_variable CV;
			bool bEntered = false;
			bool bReleased = false;
		};

		struct FBlockCookedStaticMeshInitializationCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "BlockCookedStaticMeshInitialization";
			}
		};

		std::shared_ptr<FState> State;
	};
} // namespace

TEST(FStaticMeshRenderPreparationVulkanTests,
	CookedComponentProxyConvergesFromCpuReadyGpuQueuedWithoutMutation)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory(
			"CookedStaticMeshRenderPreparationVulkan");
	std::filesystem::create_directories(Root / "Content");
	const std::filesystem::path CookRoot =
		std::filesystem::absolute(Root / "Cook");
	const std::array Mounts{
		Durin::FMountPoint{
			.VirtualRoot = "/CookedStaticMeshRenderPreparation/",
			.Owner = Durin::EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bContentWritable = true},
		Durin::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::EMountOwner::Test,
			.Root = CookRoot / "Game",
			.bAutoScan = true,
			.bContentWritable = false}};
	Durin::Testing::FScopedMountRegistryFixture MountFixture(Mounts);
	ASSERT_TRUE(MountFixture.IsValid()) << MountFixture.GetError();
	ASSERT_TRUE(Durin::InitializeAssetManager());
	Durin::FPackagePath AuthoredPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/CookedStaticMeshRenderPreparation/Mesh", AuthoredPath));
	Durin::DStaticMesh* AuthoredMesh = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(AuthoredPath, AuthoredMesh));
	ASSERT_NE(AuthoredMesh, nullptr);
	std::string Error;
	ASSERT_TRUE(AuthoredMesh->PublishImportedProduct({
		.RenderData = MakeRenderData(),
		.MaterialSlots = {
			{.Name = Durin::FName("Section0"), .SourceMaterialIndex = 0},
			{.Name = Durin::FName("Section1"), .SourceMaterialIndex = 1},
			{.Name = Durin::FName("Section2"), .SourceMaterialIndex = 2},
			{.Name = Durin::FName("Section3"), .SourceMaterialIndex = 3}}}, Error)) << Error;

	Durin::FCookContext CookContext(
		CookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*AuthoredMesh, "/Game/CookedMesh", CookContext, Error)) << Error;
	ASSERT_TRUE(CookContext.Publish(&Error)) << Error;
	ASSERT_TRUE(Durin::UnloadPackage(
		AuthoredPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();

	auto CookedConfiguration =
		Durin::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, CookedConfiguration));
	ASSERT_TRUE(Durin::InitializeAssetManager(
		std::move(CookedConfiguration)));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath CookedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedMesh", CookedPath));
	Durin::DStaticMesh* CookedMesh = nullptr;
	const Durin::FAssetResult Loaded =
		Durin::LoadObject(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
			CookedPath, AuthoredPath.GetPackageName()), CookedMesh);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(CookedMesh, nullptr);
	ASSERT_EQ(CookedMesh->GetRenderData(), nullptr);
	ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());

	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	FScopedRenderThreadBlocker RenderThreadBlocker;

	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(
		nullptr, Durin::FName("CookedStaticMeshVulkanConsumer"));
	Component->SetStaticMesh(CookedMesh);
	Component->RegisterComponent();
	EXPECT_EQ(Component->CreateSceneProxy(), nullptr);
	const auto Deadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(10);
	Durin::FCookedMeshLoadStatus LoadStatus =
		CookedMesh->RequestRenderDataAndResources();
	while (!LoadStatus.HasCpuData()
		&& std::chrono::steady_clock::now() < Deadline)
	{
		Durin::PumpCookedMeshLoadManager();
		std::this_thread::yield();
		LoadStatus = CookedMesh->RequestRenderDataAndResources();
	}
	ASSERT_TRUE(LoadStatus.HasCpuData());
	EXPECT_EQ(LoadStatus.GpuPhase, Durin::ECookedMeshGpuPhase::Queued);
	EXPECT_EQ(
		CookedMesh->GetRenderResourceStatus().Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Queued);
	auto QueuedProxy = Component->CreateSceneProxy();
	ASSERT_NE(QueuedProxy, nullptr);

	Durin::FSceneTestOwner SceneOwner;

	Durin::FScene& Scene = *SceneOwner;
	const Durin::FPrimitiveSceneId PrimitiveId(211);
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene,
		PrimitiveId, std::move(QueuedProxy), Durin::FMatrix(1.0));
	RenderThreadBlocker.Release();
	Durin::FlushRenderingCommands();
	const Durin::FCookedMeshLoadStatus ReadyStatus =
		CookedMesh->RequestRenderDataAndResources();
	EXPECT_EQ(ReadyStatus.CpuPhase, Durin::ECookedMeshCpuPhase::CpuReady);
	EXPECT_EQ(ReadyStatus.GpuPhase, Durin::ECookedMeshGpuPhase::Ready);
	EXPECT_TRUE(CookedMesh->GetRenderResourceStatus().IsReady());

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetStaticMeshSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			ASSERT_EQ(Prepared.Primitives.size(), 1u);
			EXPECT_EQ(Prepared.GetNumSections(), 1u);
			EXPECT_EQ(Prepared.RejectedPrimitives, 0u);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, PrimitiveId);
	Durin::FlushRenderingCommands();

	Component->UnregisterComponent();
	Component->SetStaticMesh(nullptr);
	Durin::MarkAsGarbage(Component);
	Durin::ShutdownCookedMeshLoadManager();
	ASSERT_TRUE(Durin::UnloadPackage(CookedPath));
	Durin::CollectGarbage();
	Durin::FlushRenderingCommands();
	SceneOwner.Reset();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::Testing::RemoveTestWorkDirectory(Root);
}

TEST(FStaticMeshRenderPreparationVulkanTests, ClassifiesResolvedSectionsAndRecomputesPerViewFacts)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	auto RenderData = MakeRenderData();
	auto MultiLODRenderData = MakeMultiLODRenderData();
	auto Opaque = MakeMaterial(Durin::EMaterialBlendMode::Opaque);
	auto OpaqueTwoSided = MakeMaterial(
		Durin::EMaterialBlendMode::Opaque, true);
	auto Masked = MakeMaterial(Durin::EMaterialBlendMode::Masked, true);
	auto Translucent = MakeMaterial(Durin::EMaterialBlendMode::Translucent);
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	auto Summary = std::make_shared<FPreparationSummary>();
	Durin::EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(RenderData->InitResources(CommandList));
			ASSERT_TRUE(MultiLODRenderData->InitResources(CommandList));
			ASSERT_TRUE(RenderData->IsReadyForRendering());
			EXPECT_TRUE(std::ranges::all_of(
				MultiLODRenderData->LODResources,
				[](const Durin::FStaticMeshLODResources& LOD) {
					return LOD.bReadyForRendering;
				}));
			auto& PublishedSection = RenderData->LODResources[0].Sections[0];
			const uint32 PublishedIndexCount = PublishedSection.IndexCount;
			PublishedSection.IndexCount = 1;
			EXPECT_TRUE(RenderData->IsReadyForRendering())
				<< "readiness must use initialization-time geometry validation";
			PublishedSection.IndexCount = PublishedIndexCount;
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FRendererModule SceneFactory;
	Durin::FScenePtr SplineSceneOwner = SceneFactory.CreateScene();
	auto& SplineScene = static_cast<Durin::FScene&>(*SplineSceneOwner);
	Durin::FSplineMeshRenderDynamicData SplineDynamic{
		.Params = {},
		.LocalBounds = Durin::FBox({-2.0, -2.0, -1.0}, {120.0, 40.0, 20.0}),
		.Revision = 1};
	SplineDynamic.Params.EndPosition = {100.0, 30.0, 10.0};
	SplineDynamic.Params.EndTangent = {80.0, 0.0, 10.0};
	SplineDynamic.Params.SourceForwardMin = -1.0;
	SplineDynamic.Params.SourceForwardMax = 1.0;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(SplineScene, Durin::FPrimitiveSceneId(72),
		std::make_unique<Durin::FSplineMeshSceneProxy>(RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent, Opaque},
			1, SplineDynamic), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&SplineScene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(CommandList, {},
					Durin::FSceneView{}, Durin::ERasterMode::Solid,
					SplineScene.GetSplineMeshSceneInfos());
			ASSERT_EQ(Prepared.Primitives.size(), 1u);
			EXPECT_EQ(Prepared.Primitives[0].VertexDomain,
				Durin::EVertexDeformationDomain::Spline);
			EXPECT_EQ(Prepared.Primitives[0].SplineDynamicData.Revision, 1u);
			ASSERT_FALSE(Prepared.Opaque.empty());
			EXPECT_EQ(Prepared.Opaque[0].PipelineKey.VertexDomain,
				Durin::EVertexDeformationDomain::Spline);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::ReleaseScene(SplineSceneOwner);
	Durin::FlushRenderingCommands();
	const Durin::FPrimitiveSceneId Id(71);
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(RenderData.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 1), Durin::Math::ScaleMatrix(Durin::FVector3(-1.0, 1.0, 1.0)));
	Durin::FlushRenderingCommands();

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene, Summary](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FSceneView FirstView;
			FirstView.ViewLocation = Durin::FVector3(0.0, 0.0, -10.0);
			const Durin::FPreparedStaticMeshView First =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetStaticMeshSceneInfos(), FirstView,
					Durin::ERasterMode::Wireframe
				);
			Summary->Opaque = First.Opaque.size();
			Summary->Masked = First.Masked.size();
			Summary->Translucent = First.Translucent.size();
			ASSERT_EQ(First.Opaque.size(), 2u);
			ASSERT_EQ(First.Masked.size(), 1u);
			ASSERT_EQ(First.Translucent.size(), 1u);
			std::array OpaqueSections{
				First.Opaque[0].SectionIndex, First.Opaque[1].SectionIndex};
			std::ranges::sort(OpaqueSections);
			EXPECT_EQ(OpaqueSections, (std::array<uint32_t, 2>{0u, 3u}));
			EXPECT_EQ(First.Masked[0].SectionIndex, 1u);
			EXPECT_EQ(First.Translucent[0].SectionIndex, 2u);
			EXPECT_EQ(
				First.Translucent[0].ShaderMapIdentity,
				First.Translucent[0].PipelineKey.Material.ShaderMap
			);
			Summary->MirroredFrontFace =
				First.Opaque.front().PipelineKey.Rasterizer.FrontFace;
			Summary->bTranslucentDepthWrite =
				First.Translucent.front().PipelineKey.Depth.bEnableWrite;
			Summary->bTranslucentBlend =
				First.Translucent.front().PipelineKey.ColorBlend.bEnable;
			Summary->FirstViewDistance =
				First.Translucent.front().TranslucentDistanceSquared;

			Durin::FSceneView SecondView;
			SecondView.ViewLocation = Durin::FVector3(0.0, 0.0, 5.0);
			const Durin::FPreparedStaticMeshView Second =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetStaticMeshSceneInfos(), SecondView,
					Durin::ERasterMode::Solid
				);
			ASSERT_EQ(Second.GetNumSections(), First.GetNumSections());
			Summary->SecondViewDistance =
				Second.Translucent.front().TranslucentDistanceSquared;
			EXPECT_EQ(Second.Opaque.front().PipelineKey.Rasterizer.PolygonMode, Durin::ERHIPolygonMode::Fill);
		}
	);
	Durin::FlushRenderingCommands();

	EXPECT_EQ(Summary->Opaque, 2u);
	EXPECT_EQ(Summary->Masked, 1u);
	EXPECT_EQ(Summary->Translucent, 1u);
	EXPECT_EQ(Summary->MirroredFrontFace, Durin::ERHIFrontFace::CounterClockwise);
	EXPECT_FALSE(Summary->bTranslucentDepthWrite);
	EXPECT_TRUE(Summary->bTranslucentBlend);
	EXPECT_NE(Summary->FirstViewDistance, Summary->SecondViewDistance);

	for (Durin::FStaticMeshSection& Section :
		 RenderData->LODResources[0].Sections)
	{
		Section.LocalBounds = Durin::FBox(
			Durin::FVector3(-1.0), Durin::FVector3(1.0));
	}
	Durin::FSceneTestOwner OrderingSceneOwner;
	Durin::FScene& OrderingScene = *OrderingSceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(OrderingScene,
		Durin::FPrimitiveSceneId(90),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>(4, Translucent),
			1),
		Durin::Math::TranslationMatrix(Durin::FVector3(0.0, 0.0, 20.0)));
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(OrderingScene,
		Durin::FPrimitiveSceneId(80),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>(4, Translucent),
			1),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&OrderingScene](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FSceneView OriginView;
			const Durin::FPreparedStaticMeshView FromOrigin =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, OrderingScene.GetStaticMeshSceneInfos(), OriginView,
					Durin::ERasterMode::Solid);
			ASSERT_EQ(FromOrigin.Translucent.size(), 8u);
			for (uint32 Index = 0; Index < 4; ++Index)
			{
				EXPECT_EQ(
					FromOrigin.Primitives[
						FromOrigin.Translucent[Index].PrimitiveIndex]
						.PrimitiveId.Value,
					90u);
				EXPECT_EQ(
					FromOrigin.Translucent[Index].SectionIndex, Index);
				EXPECT_EQ(
					FromOrigin.Primitives[
						FromOrigin.Translucent[Index + 4].PrimitiveIndex]
						.PrimitiveId.Value,
					80u);
				EXPECT_EQ(
					FromOrigin.Translucent[Index + 4].SectionIndex, Index);
			}

			Durin::FSceneView MovedView;
			MovedView.ViewLocation = Durin::FVector3(0.0, 0.0, 30.0);
			const Durin::FPreparedStaticMeshView FromMovedCamera =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, OrderingScene.GetStaticMeshSceneInfos(), MovedView,
					Durin::ERasterMode::Solid);
			ASSERT_EQ(FromMovedCamera.Translucent.size(), 8u);
			EXPECT_EQ(
				FromMovedCamera.Primitives[
					FromMovedCamera.Translucent.front().PrimitiveIndex]
					.PrimitiveId.Value,
				80u);
			EXPECT_EQ(
				FromMovedCamera.Primitives[
					FromMovedCamera.Translucent.back().PrimitiveIndex]
					.PrimitiveId.Value,
				90u);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(OrderingScene, Durin::FPrimitiveSceneId(80));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(OrderingScene, Durin::FPrimitiveSceneId(90));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner GroupingSceneOwner;

	Durin::FScene& GroupingScene = *GroupingSceneOwner;
	auto AddGroupingPrimitive = [&](uint64 PrimitiveId) {
		Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(GroupingScene,
			Durin::FPrimitiveSceneId(PrimitiveId),
			std::make_unique<Durin::FStaticMeshSceneProxy>(
				RenderData.get(),
				std::vector<Durin::FMaterialRenderProxyRef>{
					Opaque, OpaqueTwoSided, Opaque, OpaqueTwoSided},
				1),
			Durin::FMatrix(1.0));
	};
	AddGroupingPrimitive(100);
	AddGroupingPrimitive(200);
	Durin::FlushRenderingCommands();
	auto GroupedOrder = std::make_shared<
		std::vector<std::pair<uint64, uint32>>>();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&GroupingScene, GroupedOrder](
			Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, GroupingScene.GetStaticMeshSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			ASSERT_EQ(Prepared.Opaque.size(), 8u);
			EXPECT_LT(
				Prepared.OpaqueStateGroups, Prepared.OpaqueInputStateGroups);
			EXPECT_EQ(Prepared.OpaqueStateGroups, 2u);
			EXPECT_EQ(Prepared.MaskedStateGroups, 0u);
			EXPECT_EQ(Prepared.OpaqueSections, 8u);
			EXPECT_EQ(Prepared.OpaqueTriangles, 8u);
			EXPECT_EQ(Prepared.SelectedSections,
				Prepared.OpaqueSections + Prepared.MaskedSections
					+ Prepared.TranslucentSections);
			for (const Durin::FPreparedStaticMeshDraw& Draw : Prepared.Opaque)
			{
				GroupedOrder->emplace_back(
					Prepared.Primitives[Draw.PrimitiveIndex].PrimitiveId.Value,
					Draw.SectionIndex);
			}
			const Durin::FPreparedStaticMeshView Repeated =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, GroupingScene.GetStaticMeshSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			ASSERT_EQ(Repeated.Opaque.size(), Prepared.Opaque.size());
			for (size_t Index = 0; Index < Prepared.Opaque.size(); ++Index)
			{
				EXPECT_EQ(
					Repeated.Opaque[Index].SortKey,
					Prepared.Opaque[Index].SortKey);
			}
			EXPECT_EQ(
				Repeated.OpaqueInputStateGroups,
				Prepared.OpaqueInputStateGroups);
			EXPECT_EQ(Repeated.OpaqueStateGroups, Prepared.OpaqueStateGroups);
			EXPECT_EQ(
				Repeated.PipelineTransitions, Prepared.PipelineTransitions);
			EXPECT_EQ(
				Repeated.MaterialTransitions, Prepared.MaterialTransitions);
			EXPECT_EQ(
				Repeated.GeometryTransitions, Prepared.GeometryTransitions);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveSceneId(100));
	Durin::FlushRenderingCommands();
	AddGroupingPrimitive(100);
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&GroupingScene, GroupedOrder](
			Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Readded =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, GroupingScene.GetStaticMeshSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			std::vector<std::pair<uint64, uint32>> ReaddedOrder;
			for (const Durin::FPreparedStaticMeshDraw& Draw : Readded.Opaque)
			{
				ReaddedOrder.emplace_back(
					Readded.Primitives[Draw.PrimitiveIndex].PrimitiveId.Value,
					Draw.SectionIndex);
			}
			EXPECT_EQ(ReaddedOrder, *GroupedOrder);
			EXPECT_EQ(Readded.OpaqueStateGroups, 2u);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveSceneId(100));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveSceneId(200));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner MultiLODSceneOwner;

	Durin::FScene& MultiLODScene = *MultiLODSceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(MultiLODScene,
		Durin::FPrimitiveSceneId(101),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque},
			1),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&MultiLODScene, &MultiLODRenderData](
			Durin::FRHICommandListImmediate& CommandList) {
			auto MakeOrthographicView = [](double HalfExtent) {
				Durin::FSceneView View;
				View.ProjectionMatrix = Durin::FMatrix(0.0);
				View.ProjectionMatrix[1][0] = 1.0 / HalfExtent;
				View.ProjectionMatrix[2][1] = -1.0 / HalfExtent;
				View.ProjectionMatrix[0][2] = 0.1;
				View.ProjectionMatrix[3][2] = -0.1;
				View.ProjectionMatrix[3][3] = 1.0;
				View.ViewProjectionMatrix = View.ProjectionMatrix;
				View.ViewportWidth = 800;
				View.ViewportHeight = 800;
				return View;
			};
			auto MakePerspectiveView = [](
				double CameraX, uint32 Width, uint32 Height) {
				Durin::FSceneView View;
				View.ViewLocation = {CameraX, 0.0, 0.0};
				View.ViewMatrix[3][0] = -CameraX;
				const double Aspect = static_cast<double>(Width) / Height;
				View.ProjectionMatrix = Durin::FMatrix(0.0);
				View.ProjectionMatrix[1][0] = 1.0 / Aspect;
				View.ProjectionMatrix[2][1] = -1.0;
				View.ProjectionMatrix[0][2] = 1.1;
				View.ProjectionMatrix[3][2] = -1.1;
				View.ProjectionMatrix[0][3] = 1.0;
				View.ViewProjectionMatrix =
					View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportWidth = Width;
				View.ViewportHeight = Height;
				return View;
			};
			auto Prepare = [&](Durin::FSceneView View) {
				return Durin::PrepareStaticMeshView_RenderThread(
					CommandList, MultiLODScene.GetStaticMeshSceneInfos(), View,
					Durin::ERasterMode::Solid);
			};

			const Durin::FPreparedStaticMeshView Equality =
				Prepare(MakeOrthographicView(2.0));
			ASSERT_EQ(Equality.Primitives.size(), 1u);
			EXPECT_EQ(Equality.Primitives[0].RequestedLODIndex, 0u);
			EXPECT_EQ(Equality.Primitives[0].SelectedLODIndex, 0u);
			EXPECT_EQ(Equality.SelectedTriangles, 4u);
			EXPECT_EQ(Equality.SelectedSections, 1u);
			EXPECT_EQ(Equality.SelectedLODHistogram,
				(std::vector<size_t>{1u, 0u, 0u}));

			const Durin::FPreparedStaticMeshView Middle =
				Prepare(MakeOrthographicView(2.5));
			ASSERT_EQ(Middle.Primitives.size(), 1u);
			EXPECT_EQ(Middle.Primitives[0].RequestedLODIndex, 1u);
			EXPECT_EQ(Middle.Primitives[0].SelectedLODIndex, 1u);
			EXPECT_EQ(Middle.SelectedTriangles, 2u);
			EXPECT_EQ(Middle.Opaque[0].PrimitiveIndex, 0u);
			EXPECT_EQ(
				Middle.Primitives[Middle.Opaque[0].PrimitiveIndex].LOD,
				&MultiLODRenderData->LODResources[1]);
			EXPECT_EQ(
				Middle.Primitives[Middle.Opaque[0].PrimitiveIndex].VertexFactory,
				&MultiLODRenderData->LODVertexFactories[1].VertexFactory);

			const Durin::FPreparedStaticMeshView Small =
				Prepare(MakeOrthographicView(5.0));
			ASSERT_EQ(Small.Primitives.size(), 1u);
			EXPECT_EQ(Small.Primitives[0].SelectedLODIndex, 2u);
			EXPECT_EQ(Small.SelectedTriangles, 1u);

			Durin::FSceneView Forced = MakeOrthographicView(5.0);
			Forced.Settings.Mode.LODMode = Durin::EViewLODMode::ForceLOD0;
			const Durin::FPreparedStaticMeshView ForcedLOD0 = Prepare(Forced);
			ASSERT_EQ(ForcedLOD0.Primitives.size(), 1u);
			EXPECT_EQ(ForcedLOD0.Primitives[0].RequestedLODIndex, 0u);
			EXPECT_EQ(ForcedLOD0.Primitives[0].SelectedLODIndex, 0u);
			EXPECT_EQ(ForcedLOD0.SelectedTriangles, 4u);

			const Durin::FPreparedStaticMeshView PerspectiveLarge =
				Prepare(MakePerspectiveView(1.0, 1600, 800));
			const Durin::FPreparedStaticMeshView PerspectiveSmall =
				Prepare(MakePerspectiveView(1.0, 800, 400));
			ASSERT_EQ(PerspectiveLarge.Primitives.size(), 1u);
			ASSERT_EQ(PerspectiveSmall.Primitives.size(), 1u);
			EXPECT_EQ(PerspectiveLarge.Primitives[0].SelectedLODIndex, 1u);
			EXPECT_EQ(
				PerspectiveLarge.Primitives[0].SelectedLODIndex,
				PerspectiveSmall.Primitives[0].SelectedLODIndex);
			EXPECT_EQ(
				Prepare(MakePerspectiveView(2.9, 800, 800))
					.Primitives[0].SelectedLODIndex,
				1u);
			EXPECT_EQ(
				Prepare(MakePerspectiveView(3.0, 800, 800))
					.Primitives[0].SelectedLODIndex,
				0u);
			EXPECT_EQ(
				Prepare(MakePerspectiveView(3.1, 800, 800))
					.Primitives[0].SelectedLODIndex,
				0u);
			const Durin::FPreparedStaticMeshView NearCrossing =
				Prepare(MakePerspectiveView(4.5, 800, 800));
			ASSERT_EQ(NearCrossing.Primitives.size(), 1u);
			EXPECT_EQ(NearCrossing.Primitives[0].SelectedLODIndex, 0u);
			EXPECT_EQ(NearCrossing.ProjectedSizeFallbacks, 1u);

			MultiLODRenderData->LODVertexFactories[1]
				.VertexFactory.ReleaseResource();
			MultiLODRenderData->LODResources[1].bReadyForRendering = false;
			const Durin::FPreparedStaticMeshView Fallback =
				Prepare(MakeOrthographicView(2.5));
			ASSERT_EQ(Fallback.Primitives.size(), 1u);
			EXPECT_EQ(Fallback.Primitives[0].RequestedLODIndex, 1u);
			EXPECT_EQ(Fallback.Primitives[0].SelectedLODIndex, 2u);
			EXPECT_EQ(Fallback.ResourceFallbacks, 1u);
			EXPECT_EQ(Fallback.SelectedTriangles, 1u);
			EXPECT_EQ(Fallback.VisibleCandidates,
				Fallback.Primitives.size() + Fallback.RejectedPrimitives);
			MultiLODRenderData->LODVertexFactories[0]
				.VertexFactory.ReleaseResource();
			MultiLODRenderData->LODResources[0].bReadyForRendering = false;
			MultiLODRenderData->LODVertexFactories[2]
				.VertexFactory.ReleaseResource();
			MultiLODRenderData->LODResources[2].bReadyForRendering = false;
			const Durin::FPreparedStaticMeshView Unavailable =
				Prepare(MakeOrthographicView(2.5));
			EXPECT_TRUE(Unavailable.Primitives.empty());
			EXPECT_EQ(Unavailable.RejectedPrimitives, 1u);
			EXPECT_TRUE(Unavailable.SelectedLODHistogram.empty());
			ASSERT_TRUE(MultiLODRenderData->InitResources(CommandList));
			const Durin::FPreparedStaticMeshView Retried =
				Prepare(MakeOrthographicView(2.5));
			ASSERT_EQ(Retried.Primitives.size(), 1u);
			EXPECT_EQ(Retried.Primitives[0].SelectedLODIndex, 1u);
			EXPECT_EQ(Retried.ResourceFallbacks, 0u);
			EXPECT_EQ(
				Retried.SelectedSections,
				Retried.Opaque.size() + Retried.Masked.size()
					+ Retried.Translucent.size());
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(MultiLODScene,
		Durin::FPrimitiveSceneId(101),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque},
			2),
		Durin::Math::ScaleMatrix(Durin::FVector3(1.0, 0.5, 2.0)));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&MultiLODScene](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FSceneView View;
			View.ProjectionMatrix = Durin::FMatrix(0.0);
			View.ProjectionMatrix[1][0] = 0.2;
			View.ProjectionMatrix[2][1] = -0.2;
			View.ProjectionMatrix[0][2] = 0.1;
			View.ProjectionMatrix[3][2] = -0.1;
			View.ProjectionMatrix[3][3] = 1.0;
			View.ViewProjectionMatrix = View.ProjectionMatrix;
			View.ViewportWidth = 800;
			View.ViewportHeight = 800;
			const Durin::FPreparedStaticMeshView Nonuniform =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, MultiLODScene.GetStaticMeshSceneInfos(), View,
					Durin::ERasterMode::Solid);
			ASSERT_EQ(Nonuniform.Primitives.size(), 1u);
			EXPECT_EQ(Nonuniform.Primitives[0].SelectedLODIndex, 1u);
			const Durin::FMatrix4f& NormalToWorld =
				Nonuniform.Primitives[0].NormalToWorld;
			EXPECT_FLOAT_EQ(NormalToWorld[0][0], 1.0f);
			EXPECT_FLOAT_EQ(NormalToWorld[1][1], 2.0f);
			EXPECT_FLOAT_EQ(NormalToWorld[2][2], 0.5f);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(MultiLODScene, Durin::FPrimitiveSceneId(101));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner DegenerateTransformSceneOwner;

	Durin::FScene& DegenerateTransformScene = *DegenerateTransformSceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(DegenerateTransformScene,
		Durin::FPrimitiveSceneId(102),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 2),
		Durin::Math::ScaleMatrix(Durin::FVector3(0.0, 1.0, 1.0)));
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(DegenerateTransformScene,
		Durin::FPrimitiveSceneId(103),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 2),
		Durin::Math::ScaleMatrix(Durin::FVector3(1.0e-12, 1.0, 1.0)));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&DegenerateTransformScene](
			Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList,
					DegenerateTransformScene.GetStaticMeshSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			EXPECT_TRUE(Prepared.Primitives.empty());
			EXPECT_EQ(Prepared.GetNumSections(), 0u);
			EXPECT_EQ(Prepared.RejectedPrimitives, 2u);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(DegenerateTransformScene, Durin::FPrimitiveSceneId(102));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(DegenerateTransformScene, Durin::FPrimitiveSceneId(103));
	Durin::FlushRenderingCommands();

	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(RenderData.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 2), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Replaced =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetStaticMeshSceneInfos(), Durin::FSceneView{},
					Durin::ERasterMode::Solid
				);
			ASSERT_EQ(Replaced.GetNumSections(), 4u);
			EXPECT_EQ(Replaced.Opaque.front().PipelineKey.Rasterizer.FrontFace, Durin::ERHIFrontFace::Clockwise);
		}
	);
	Durin::FlushRenderingCommands();

	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Id);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetStaticMeshSceneInfos().empty());
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			EXPECT_EQ(Durin::PrepareStaticMeshView_RenderThread(
				CommandList, Scene.GetStaticMeshSceneInfos(), Durin::FSceneView{},
				Durin::ERasterMode::Solid).GetNumSections(), 0u);
			RenderData->ReleaseResources();
			MultiLODRenderData->ReleaseResources();
			EXPECT_TRUE(std::ranges::none_of(
				MultiLODRenderData->LODResources,
				[](const Durin::FStaticMeshLODResources& LOD) {
					return LOD.bReadyForRendering;
				}));
		}
	);
	Durin::FlushRenderingCommands();
	DegenerateTransformSceneOwner.Reset();
	MultiLODSceneOwner.Reset();
	GroupingSceneOwner.Reset();
	OrderingSceneOwner.Reset();
	SceneOwner.Reset();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}
