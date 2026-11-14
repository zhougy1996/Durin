#include "ComponentPSOPrecacheTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "StaticMeshRenderPreparationTestSupport.h"
#include "StaticMeshTestAccess.h"
#include "PSOPrecache.h"
#include "PSOPrecacheComponent.h"
#include "RenderPipelineCreation.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/InstanceCulling.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Materials/MaterialShaderMap.h"

#include "GeometrySubmissionTestSupport.h"
#include "Materials/MaterialExpressions.h"
#include "Asset/AssetCompilingManager.h"
#include "LightSceneTestSupport.h"
#include "Renderers/SceneRendererProfiling.h"
#include "ShaderBuild/ShaderPaths.h"
#include "Shader/IShaderBuildModule.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Misc/FileHelper.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "SceneView.h"
#include "Asset/PackageSerialization.h"
#include "VulkanEngineTestSupport.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Asset/CookedMeshLoadManagerPrivate.h"
#include "StaticMesh/StaticMeshLoadUpdate.h"
#include "Asset/Load.h"
#include "Asset/AssetCook.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/MeshBatch.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Rendering/MeshGeometryRecord.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialRenderProxy.h"
#include "Math/Operations.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "RenderResourcePreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/SceneViewPreparation.h"
#include "SceneTestAccess.h"
#include "SceneInfo.h"
#include "GPUScene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"

#include "NativeDObjectTestSupport.h"

namespace
{
	using namespace Durin::Tests::Preparation;

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
	BlockingMeshCpuResidencyDoesNotInitializeGpuResources)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	InitializeDObjectSystem();
	auto* StaticMesh = Durin::NewObject<Durin::DStaticMesh>(nullptr, "CpuOnlyStaticMesh");
	EXPECT_FALSE(StaticMesh->HasPendingRenderResourceInitialization());
	std::string Error;
	ASSERT_TRUE(Durin::FStaticMeshTestAccess::ReplaceRenderData(StaticMesh, MakeRenderData(), {
		{.Name = Durin::FName("Section0"), .SourceMaterialIndex = 0},
		{.Name = Durin::FName("Section1"), .SourceMaterialIndex = 1},
		{.Name = Durin::FName("Section2"), .SourceMaterialIndex = 2},
		{.Name = Durin::FName("Section3"), .SourceMaterialIndex = 3}}));
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	auto CheckCpuOnlyAndGpuInitialization = [](auto* Mesh) {
		const auto Before = Mesh->GetRenderResourceStatus();
		const auto Loaded = Mesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(static_cast<bool>(Loaded)) << Durin::FormatCookedMeshLoadError(Loaded.error());
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Readiness, Durin::EStaticMeshRenderResourceReadiness::Unavailable);
		EXPECT_FALSE(Mesh->HasPendingRenderResourceInitialization());
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Before.Revision);
		const auto Repeated = Mesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(static_cast<bool>(Repeated)) << Durin::FormatCookedMeshLoadError(Repeated.error());
		EXPECT_FALSE(Mesh->HasPendingRenderDataLoad());
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Before.Revision);
		auto* Spline = Durin::NewObject<Durin::DSplineMeshComponent>(nullptr, "GpuReadinessSpline");
		std::shared_ptr<const Durin::FSplineMeshDerivedState> SplineState;
		{
			FScopedRenderThreadBlocker Blocker;
			Mesh->InitResources();
			Spline->SetStaticMesh(Mesh);
			SplineState = Spline->GetDerivedState();
			ASSERT_TRUE(SplineState->IsValid());
			const auto Queued = Mesh->GetRenderResourceStatus();
			const auto WhileQueued = Mesh->EnsureRenderDataLoadedBlocking();
			EXPECT_TRUE(static_cast<bool>(WhileQueued));
			EXPECT_EQ(Mesh->GetRenderResourceStatus().Readiness, Durin::EStaticMeshRenderResourceReadiness::Queued);
			EXPECT_FALSE(Mesh->GetRenderResourceStatus().IsReady());
			EXPECT_TRUE(Mesh->HasPendingRenderResourceInitialization());
			Mesh->InitResources();
			EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Queued.Revision);
		}
		Durin::FlushRenderingCommands();
		const auto Ready = Mesh->GetRenderResourceStatus();
		EXPECT_TRUE(Ready.IsReady());
		EXPECT_FALSE(Mesh->HasPendingRenderResourceInitialization());
		Mesh->InitResources();
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Ready.Revision);
		EXPECT_EQ(Spline->GetDerivedState(), SplineState);
		EXPECT_EQ(Spline->ContainsPhysicsTriMeshData(), Mesh->ContainsPhysicsTriMeshData());
		if (Mesh->ContainsPhysicsTriMeshData()) EXPECT_TRUE(Spline->GetPhysicsTriMeshData().has_value());
		Durin::MarkAsGarbage(Spline);
	};
	CheckCpuOnlyAndGpuInitialization(StaticMesh);
	Durin::MarkAsGarbage(StaticMesh);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}

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
			.bContentWritable = true}};
	Durin::Testing::FScopedMountRegistryFixture MountFixture(Mounts);
	ASSERT_TRUE(MountFixture.IsValid()) << MountFixture.GetError();
	ASSERT_TRUE(Durin::InitializeAssetRuntime());
	Durin::FPackagePath AuthoredPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/CookedStaticMeshRenderPreparation/Mesh", AuthoredPath));
	Durin::DStaticMesh* AuthoredMesh = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(AuthoredPath, AuthoredMesh));
	ASSERT_NE(AuthoredMesh, nullptr);
	std::string Error;
	ASSERT_TRUE(Durin::FStaticMeshTestAccess::ReplaceRenderData(AuthoredMesh, MakeRenderData(), {
			{.Name = Durin::FName("Section0"), .SourceMaterialIndex = 0},
			{.Name = Durin::FName("Section1"), .SourceMaterialIndex = 1},
			{.Name = Durin::FName("Section2"), .SourceMaterialIndex = 2},
			{.Name = Durin::FName("Section3"), .SourceMaterialIndex = 3}}));

	Durin::FCookContext CookContext(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::SaveCookPackage((*AuthoredMesh).GetPackage(), "/Game/CookedMesh", CookContext)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(CookContext, CookRoot)) << Error;
	ASSERT_TRUE(Durin::UnloadPackage(
		AuthoredPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	Durin::ShutdownAssetRuntime();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);

	const std::array CookedMounts{
		Durin::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::EMountOwner::Test,
			.Root = CookRoot / "Game",
			.bAutoScan = true,
			.bContentWritable = false}};
	Durin::Testing::FScopedMountRegistryFixture CookedMountFixture(CookedMounts);
	ASSERT_TRUE(CookedMountFixture.IsValid()) << CookedMountFixture.GetError();
	auto CookedConfiguration =
		Durin::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, CookedConfiguration));
	ASSERT_TRUE(Durin::InitializeAssetRuntime(
		std::move(CookedConfiguration)));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath CookedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedMesh", CookedPath));
	Durin::DStaticMesh* CookedMesh = nullptr;
	const auto Loaded =
		Durin::LoadObjectDetailed<Durin::DStaticMesh>(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
			CookedPath, AuthoredPath.GetPackageName()));
		CookedMesh = Loaded.value_or(nullptr);
	ASSERT_TRUE(Loaded) << (Loaded ? std::string{} : Loaded.error().Message);
	ASSERT_NE(CookedMesh, nullptr);
	ASSERT_EQ(CookedMesh->GetRenderData(), nullptr);
	ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());

	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	for (bool bUseManager : {true, false})
	{
		if (!bUseManager) Durin::ShutdownCookedMeshLoadManager();
		std::unique_ptr<Durin::FCookedMeshLoadManager> Scheduler;
		Durin::TStrongObjectPtr<Durin::DStaticMesh> Blocker;
		if (bUseManager)
		{
			Scheduler = std::make_unique<Durin::FCookedMeshLoadManager>(
				Durin::FCookedMeshLoadManagerConfig{.MaxConcurrentRequests = 1});
			ASSERT_TRUE(Scheduler->Initialize());
			Blocker = Durin::TStrongObjectPtr<Durin::DStaticMesh>(
				Durin::NewObject<Durin::DStaticMesh>(nullptr, "CpuOnlyQueueBlocker"));
			auto Bulk = Durin::FBulkData::TryCreateDetached(Durin::FByteBuffer{std::byte{0xff}});
			ASSERT_TRUE(Bulk);
			const_cast<Durin::FBulkData&>(Blocker->GetCookedRenderData()) = std::move(*Bulk);
			auto Active = std::make_shared<Durin::FStaticMeshLoadUpdate>(*Blocker, false);
			Durin::FStaticMeshTestAccess::SetPendingUpdate(Blocker.Get(), Active);
			ASSERT_TRUE(Scheduler->Submit(Active));
			Scheduler->Pump();
			auto Pending = std::make_shared<Durin::FStaticMeshLoadUpdate>(*CookedMesh, false);
			Durin::FStaticMeshTestAccess::SetPendingUpdate(CookedMesh, Pending);
			ASSERT_TRUE(Scheduler->Submit(Pending));
			EXPECT_EQ(Scheduler->GetDiagnostics().PendingRequestCount, 1u);
		}
		const auto CpuOnly = CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(static_cast<bool>(CpuOnly)) << Durin::FormatCookedMeshLoadError(CpuOnly.error());
		if (Scheduler)
		{
			for (int Pump = 0; Pump < 3; ++Pump) Scheduler->Pump();
			Scheduler->Shutdown();
		}
		EXPECT_EQ(CookedMesh->GetRenderResourceStatus().Readiness, Durin::EStaticMeshRenderResourceReadiness::Unavailable);
		EXPECT_EQ(CookedMesh->GetRenderData()->GetNumInitializedResources(), 0u);
		ASSERT_TRUE(Durin::UnloadPackage(CookedPath));
		CookedMesh = nullptr;
		{
			auto LoadedValue = Durin::LoadObjectDetailed<Durin::DStaticMesh>(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
			CookedPath, AuthoredPath.GetPackageName()));
			CookedMesh = LoadedValue.value_or(nullptr);
			ASSERT_TRUE(LoadedValue);
		}
		ASSERT_EQ(CookedMesh->GetRenderData(), nullptr);
	}
	ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
	FScopedRenderThreadBlocker RenderThreadBlocker;

	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(
		nullptr, Durin::FName("CookedStaticMeshVulkanConsumer"));
	Component->SetStaticMesh(CookedMesh);
	Component->RegisterComponent();
	EXPECT_EQ(Component->CreateSceneProxy(), nullptr);
	EXPECT_FALSE(CookedMesh->HasPendingRenderResourceInitialization());
	const auto Deadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(10);
	CookedMesh->RequestRenderDataAndResources();
	while (!CookedMesh->GetRenderData()
		&& std::chrono::steady_clock::now() < Deadline)
	{
		Durin::PumpCookedMeshLoadManager();
		std::this_thread::yield();
		CookedMesh->RequestRenderDataAndResources();
	}
	ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
	EXPECT_EQ(
		CookedMesh->GetRenderResourceStatus().Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Queued);
	auto QueuedProxy = Component->CreateSceneProxy();
	ASSERT_NE(QueuedProxy, nullptr);

	Durin::FSceneTestOwner SceneOwner;

	Durin::FScene& Scene = *SceneOwner;
	const Durin::FPrimitiveComponentId PrimitiveId(211);
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene,
		PrimitiveId, std::move(QueuedProxy), Durin::FMatrix(1.0));
	RenderThreadBlocker.Release();
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(CookedMesh->RequestRenderDataAndResources());
	EXPECT_NE(CookedMesh->GetRenderData(), nullptr);
	EXPECT_EQ(CookedMesh->GetRenderResourceStatus().Readiness, Durin::EStaticMeshRenderResourceReadiness::Ready);
	EXPECT_TRUE(CookedMesh->GetRenderResourceStatus().IsReady());

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			ASSERT_EQ(Prepared.Primitives.size(), 1u);
			// MakeRenderData supplies four sections on the single cooked primitive.
			EXPECT_EQ(Prepared.GetNumSections(), 4u);
			EXPECT_EQ(Prepared.RejectedPrimitives, 0u);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(Scene, PrimitiveId);
	Durin::FlushRenderingCommands();

	Component->UnregisterComponent();
	Component->SetStaticMesh(nullptr);
	Durin::MarkAsGarbage(Component);
	Durin::ShutdownCookedMeshLoadManager();
	ASSERT_TRUE(Durin::UnloadPackage(CookedPath));
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::FlushRenderingCommands();
	SceneOwner.Reset();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
	Durin::ShutdownAssetRuntime();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::InitializeAssetRuntime();
}

TEST(FStaticMeshRenderPreparationVulkanTests, ClassifiesResolvedSectionsAndRecomputesPerViewFacts)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::InitializeAssetRuntime());
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
			const auto PublishedId = RenderData->LODResources[0].GeometryRecord->GetRecordId();
			ASSERT_TRUE(RenderData->InitResources(CommandList));
			EXPECT_EQ(RenderData->LODResources[0].GeometryRecord->GetRecordId(), PublishedId);
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
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate&) {
			using namespace Durin;
			FMeshCollectionContext Context;
			Context.PrimitiveId = FPrimitiveComponentId(700);
			Context.WorldBounds = RenderData->LocalBounds;
			Context.ProjectionStatus = EMeshCollectionProjectionStatus::Valid;
			FStaticMeshSceneProxy StaticProxy(RenderData.get(), {Opaque, Masked, Translucent, Opaque});
			FMeshBatchCollector Before(EMeshCollectionPurpose::Receiver);
			StaticProxy.CollectMeshBatches(Context, Before);
			ASSERT_EQ(Before.GetBatches().size(), 1u);
			const auto& Original = Before.GetBatches().front();
			ASSERT_TRUE(Original.GeometryRecord);
			EXPECT_NE(Original.GeometryRecord->GetRecordId(), 0u);
			EXPECT_EQ(Original.GeometryRecord, RenderData->LODResources[0].GeometryRecord);
			FMeshBatchCollector Repeated(EMeshCollectionPurpose::Receiver);
			StaticProxy.CollectMeshBatches(Context, Repeated);
			ASSERT_EQ(Repeated.GetBatches().size(), 1u);
			EXPECT_EQ(Original.GeometryRecord, Repeated.GetBatches()[0].GeometryRecord);
			EXPECT_EQ(Original.Binding, Repeated.GetBatches()[0].Binding);
			ASSERT_EQ(Original.GetNumElements(), 4u);
			EXPECT_TRUE(Original.Elements.empty());
			EXPECT_EQ(&Original.GetElement(1).Draw, &Original.GeometryRecord->GetElements()[1].Draw);
			EXPECT_EQ(Original.GetElement(1).Draw.FirstElement, 3u);
			EXPECT_EQ(Original.GetElement(1).Material.PlanningPassIdentity.ShaderMap.BlendMode, EMaterialBlendMode::Masked);
			StaticProxy.UpdateMaterialBinding_RenderThread({0, Translucent});
			FMeshBatchCollector After(EMeshCollectionPurpose::Receiver);
			StaticProxy.CollectMeshBatches(Context, After);
			ASSERT_EQ(After.GetBatches().size(), 1u);
			EXPECT_EQ(Original.GeometryRecord, After.GetBatches()[0].GeometryRecord);
			EXPECT_EQ(Original.Binding, After.GetBatches()[0].Binding);
			EXPECT_EQ(After.GetBatches()[0].GetElement(0).Material.PlanningPassIdentity.ShaderMap.BlendMode, EMaterialBlendMode::Translucent);
			EXPECT_EQ(Original.GetElement(0).Material.PlanningPassIdentity.ShaderMap.BlendMode, EMaterialBlendMode::Opaque);
			FSplineMeshRenderDynamicData Dynamic{.LocalBounds = Context.WorldBounds};
			FSplineMeshSceneProxy SplineProxy(RenderData.get(), {Opaque, Masked, Translucent, Opaque}, Dynamic);
			FMeshBatchCollector SplineBefore(EMeshCollectionPurpose::Receiver);
			SplineProxy.CollectMeshBatches(Context, SplineBefore);
			ASSERT_EQ(SplineBefore.GetBatches().size(), 1u);
			const auto OldBinding = std::dynamic_pointer_cast<const FSplineMeshBatchBinding>(SplineBefore.GetBatches()[0].Binding);
			ASSERT_TRUE(OldBinding);
			FMeshBatchCollector SplineRepeated(EMeshCollectionPurpose::Receiver);
			SplineProxy.CollectMeshBatches(Context, SplineRepeated);
			ASSERT_EQ(SplineRepeated.GetBatches().size(), 1u);
			EXPECT_EQ(OldBinding, SplineRepeated.GetBatches()[0].Binding);
			Dynamic.Params.EndPosition = {12.0, 34.0, 56.0};
			ASSERT_TRUE(SplineProxy.UpdateDynamicData_RenderThread(Dynamic));
			FMeshBatchCollector SplineAfter(EMeshCollectionPurpose::Shadow);
			Context.Purpose = EMeshCollectionPurpose::Shadow;
			SplineProxy.CollectMeshBatches(Context, SplineAfter);
			ASSERT_EQ(SplineAfter.GetBatches().size(), 1u);
			EXPECT_EQ(Original.GeometryRecord->GetGeometryId(), SplineAfter.GetBatches()[0].GeometryRecord->GetGeometryId());
			EXPECT_EQ(Original.GeometryRecord->GetElements().data(), SplineAfter.GetBatches()[0].GeometryRecord->GetElements().data());
			const auto NewBinding = std::dynamic_pointer_cast<const FSplineMeshBatchBinding>(SplineAfter.GetBatches()[0].Binding);
			ASSERT_TRUE(NewBinding);
			EXPECT_EQ(OldBinding->DynamicData.Params.EndPosition, FSplineMeshParams{}.EndPosition);
			EXPECT_EQ(NewBinding->DynamicData.Params.EndPosition, Dynamic.Params.EndPosition);
			EXPECT_NE(OldBinding.get(), NewBinding.get());
			auto InvalidDynamic = Dynamic;
			InvalidDynamic.LocalBounds = {};
			EXPECT_FALSE(SplineProxy.UpdateDynamicData_RenderThread(InvalidDynamic));
			FMeshBatchCollector SplineRejectedUpdate(EMeshCollectionPurpose::Shadow);
			SplineProxy.CollectMeshBatches(Context, SplineRejectedUpdate);
			ASSERT_EQ(SplineRejectedUpdate.GetBatches().size(), 1u);
			EXPECT_EQ(NewBinding, SplineRejectedUpdate.GetBatches()[0].Binding);
			Context.WorldBounds = FBox{};
			FMeshBatchCollector Invalid(EMeshCollectionPurpose::Receiver);
			StaticProxy.CollectMeshBatches(Context, Invalid);
			EXPECT_TRUE(Invalid.GetBatches().empty());
			EXPECT_EQ(Invalid.GetOutcomeCount(EGeometrySubmissionOutcome::InvalidSubmission), 1u);
			FStaticMeshSceneProxy Unavailable(nullptr, {});
			FMeshBatchCollector Missing(EMeshCollectionPurpose::Receiver);
			Unavailable.CollectMeshBatches(Context, Missing);
			EXPECT_EQ(Missing.GetOutcomeCount(EGeometrySubmissionOutcome::ResourceFailure), 1u);
		});
	Durin::FlushRenderingCommands();
	Durin::FRendererModule SceneFactory;
	Durin::FScenePtr SplineSceneOwner = SceneFactory.CreateScene();
	auto& SplineScene = static_cast<Durin::FScene&>(*SplineSceneOwner);
	Durin::FSplineMeshRenderDynamicData SplineDynamic{
		.Params = {},
		.LocalBounds = Durin::FBox({-2.0, -2.0, -1.0}, {120.0, 40.0, 20.0})};
	SplineDynamic.Params.EndPosition = {100.0, 30.0, 10.0};
	SplineDynamic.Params.EndTangent = {80.0, 0.0, 10.0};
	SplineDynamic.Params.SourceForwardMin = -1.0;
	SplineDynamic.Params.SourceForwardMax = 1.0;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(SplineScene, Durin::FPrimitiveComponentId(72),
		std::make_unique<Durin::FSplineMeshSceneProxy>(RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent, Opaque}, SplineDynamic), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&SplineScene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(CommandList, SplineScene.GetPrimitiveSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			ASSERT_EQ(Prepared.Primitives.size(), 1u);
			EXPECT_EQ(Prepared.Primitives[0].VertexDomain,
				Durin::EVertexDeformationDomain::Spline);
			EXPECT_EQ(std::dynamic_pointer_cast<const Durin::FSplineMeshBatchBinding>(Prepared.Primitives[0].CollectedBinding)->DynamicData.Params.EndPosition, Durin::FVector3(100.0, 30.0, 10.0));
			ASSERT_FALSE(Prepared.Opaque.empty());
			EXPECT_EQ(Prepared.Opaque[0].Command->PipelineKey.VertexDomain,
				Durin::EVertexDeformationDomain::Spline);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::ReleaseScene(SplineSceneOwner);
	Durin::FlushRenderingCommands();
	const Durin::FPrimitiveComponentId Id(71);
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(RenderData.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}), Durin::Math::ScaleMatrix(Durin::FVector3(-1.0, 1.0, 1.0)));
	Durin::FlushRenderingCommands();

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene, Summary](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FSceneView FirstView;
			FirstView.Settings.Mode.TranslucentSortPolicy = Durin::ETranslucentSortPolicy::Distance;
			FirstView.ViewLocation = Durin::FVector3(0.0, 0.0, -10.0);
			Durin::FStaticMeshPreparationCache SharedFacts;
			Durin::FMeshDrawDescriptionCache Commands;
			Commands.BeginSubmission();
			SharedFacts.Commands = &Commands;
			const Durin::FPreparedStaticMeshView First =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(), FirstView,
					Durin::ERasterMode::Wireframe, Durin::ERenderPreparationMode::Full, &SharedFacts
				);
			Summary->Opaque = First.Opaque.size();
			Summary->Masked = First.Masked.size();
			Summary->Translucent = First.Translucent.size();
			ASSERT_EQ(First.Opaque.size(), 2u);
			ASSERT_EQ(First.Masked.size(), 1u);
			ASSERT_EQ(First.Translucent.size(), 1u);
			EXPECT_EQ(First.DrawDescriptionBuilds, First.GetNumSections());
			Commands.EndSubmission();
			Commands.BeginSubmission();
			Durin::FStaticMeshPreparationCache NextFrameFacts;
			NextFrameFacts.Commands = &Commands;
			auto MovedView = FirstView;
			MovedView.ViewLocation = Durin::FVector3(0.0, 0.0, 30.0);
			const auto Warm = Durin::PrepareStaticMeshView_RenderThread(CommandList,
				Scene.GetPrimitiveSceneInfos(), MovedView, Durin::ERasterMode::Wireframe,
				Durin::ERenderPreparationMode::Full, &NextFrameFacts);
			EXPECT_EQ(Warm.DrawDescriptionBuilds, 0u);
			EXPECT_EQ(Warm.DrawDescriptionReuses, First.GetNumSections());
			ASSERT_EQ(Warm.Translucent.size(), 1u);
			EXPECT_EQ(Warm.Translucent[0].Command, First.Translucent[0].Command);
			EXPECT_NE(Warm.Translucent[0].TranslucentSortDepth, First.Translucent[0].TranslucentSortDepth);
			Commands.EndSubmission();
			EXPECT_EQ(Commands.Num(), First.GetNumSections());
			EXPECT_EQ(First.DynamicGeometryInputValidations, 0u);
			EXPECT_EQ(First.PublishedGeometryElements, 4u);
			std::array OpaqueSections{
				First.Opaque[0].Command->SectionIndex, First.Opaque[1].Command->SectionIndex};
			std::ranges::sort(OpaqueSections);
			EXPECT_EQ(OpaqueSections, (std::array<uint64, 2>{0u, 3u}));
			EXPECT_EQ(First.Masked[0].Command->SectionIndex, 1u);
			EXPECT_EQ(First.Translucent[0].Command->SectionIndex, 2u);
			EXPECT_EQ(
				First.Translucent[0].Command->Material.PlanningPassIdentity.ShaderMap,
				First.Translucent[0].Command->PipelineKey.Material.ShaderMap
			);
			Summary->MirroredFrontFace =
				First.Opaque.front().Command->PipelineKey.Rasterizer.FrontFace;
			Summary->bTranslucentDepthWrite =
				First.Translucent.front().Command->PipelineKey.Depth.bEnableWrite;
			Summary->bTranslucentBlend =
				First.Translucent.front().Command->PipelineKey.ColorBlend.bEnable;
			Summary->FirstViewDistance =
				First.Translucent.front().TranslucentSortDepth;
			const size_t TransformBuilds = SharedFacts.TransformBuilds;
			const size_t MaterialBuilds = SharedFacts.MaterialBuilds;
			EXPECT_GT(TransformBuilds, 0u);
			EXPECT_GT(MaterialBuilds, 0u);

			Durin::FSceneView SecondView;
			SecondView.Settings.Mode.TranslucentSortPolicy = Durin::ETranslucentSortPolicy::Distance;
			SecondView.ViewLocation = Durin::FVector3(0.0, 0.0, 5.0);
			const Durin::FPreparedStaticMeshView Second =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(), SecondView,
					Durin::ERasterMode::Solid, Durin::ERenderPreparationMode::Full, &SharedFacts
				);
			ASSERT_EQ(Second.GetNumSections(), First.GetNumSections());
			EXPECT_EQ(SharedFacts.TransformBuilds, TransformBuilds);
			EXPECT_EQ(SharedFacts.MaterialBuilds, MaterialBuilds);
			EXPECT_EQ(Second.Opaque.front().MaterialUniformIndex, First.Opaque.front().MaterialUniformIndex);
			Summary->SecondViewDistance =
				Second.Translucent.front().TranslucentSortDepth;
			EXPECT_EQ(Second.Opaque.front().Command->PipelineKey.Rasterizer.PolygonMode, Durin::ERHIPolygonMode::Fill);
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
		Durin::FPrimitiveComponentId(90),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>(4, Translucent)),
		Durin::Math::TranslationMatrix(Durin::FVector3(0.0, 0.0, 20.0)));
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(OrderingScene,
		Durin::FPrimitiveComponentId(80),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>(4, Translucent)),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&OrderingScene](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FSceneView OriginView;
			OriginView.Settings.Mode.TranslucentSortPolicy = Durin::ETranslucentSortPolicy::Distance;
			const Durin::FPreparedStaticMeshView FromOrigin =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, OrderingScene.GetPrimitiveSceneInfos(), OriginView,
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
					FromOrigin.Translucent[Index].Command->SectionIndex, Index);
				EXPECT_EQ(
					FromOrigin.Primitives[
						FromOrigin.Translucent[Index + 4].PrimitiveIndex]
						.PrimitiveId.Value,
					80u);
				EXPECT_EQ(
					FromOrigin.Translucent[Index + 4].Command->SectionIndex, Index);
			}

			Durin::FSceneView MovedView;
			MovedView.Settings.Mode.TranslucentSortPolicy = Durin::ETranslucentSortPolicy::Distance;
			MovedView.ViewLocation = Durin::FVector3(0.0, 0.0, 30.0);
			const Durin::FPreparedStaticMeshView FromMovedCamera =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, OrderingScene.GetPrimitiveSceneInfos(), MovedView,
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
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(OrderingScene, Durin::FPrimitiveComponentId(80));
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(OrderingScene, Durin::FPrimitiveComponentId(90));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner GroupingSceneOwner;

	Durin::FScene& GroupingScene = *GroupingSceneOwner;
	auto AddGroupingPrimitive = [&](uint64 PrimitiveId) {
		Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(GroupingScene,
			Durin::FPrimitiveComponentId(PrimitiveId),
			std::make_unique<Durin::FStaticMeshSceneProxy>(
				RenderData.get(),
				std::vector<Durin::FMaterialRenderProxyRef>{
					Opaque, OpaqueTwoSided, Opaque, OpaqueTwoSided}),
			Durin::FMatrix(1.0));
	};
	AddGroupingPrimitive(100);
	AddGroupingPrimitive(200);
	Durin::FlushRenderingCommands();
	auto GroupedOrder = std::make_shared<
		std::vector<std::pair<uint64, uint64>>>();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&GroupingScene, GroupedOrder](
			Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, GroupingScene.GetPrimitiveSceneInfos(),
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
					Draw.Command->SectionIndex);
			}
			const Durin::FPreparedStaticMeshView Repeated =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, GroupingScene.GetPrimitiveSceneInfos(),
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
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveComponentId(100));
	Durin::FlushRenderingCommands();
	AddGroupingPrimitive(100);
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&GroupingScene, GroupedOrder](
			Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Readded =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, GroupingScene.GetPrimitiveSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			std::vector<std::pair<uint64, uint64>> ReaddedOrder;
			for (const Durin::FPreparedStaticMeshDraw& Draw : Readded.Opaque)
			{
				ReaddedOrder.emplace_back(
					Readded.Primitives[Draw.PrimitiveIndex].PrimitiveId.Value,
					Draw.Command->SectionIndex);
			}
			EXPECT_EQ(ReaddedOrder, *GroupedOrder);
			EXPECT_EQ(Readded.OpaqueStateGroups, 2u);
		}
	);
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveComponentId(100));
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveComponentId(200));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner MultiLODSceneOwner;

	Durin::FScene& MultiLODScene = *MultiLODSceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(MultiLODScene,
		Durin::FPrimitiveComponentId(101),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}),
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
					CommandList, MultiLODScene.GetPrimitiveSceneInfos(), View,
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
				Middle.Opaque[0].Command->Indices.Buffer,
				MultiLODRenderData->LODResources[1].IndexBuffer.GetRHI());
			EXPECT_EQ(
				Middle.Primitives[Middle.Opaque[0].PrimitiveIndex].CollectedBinding->Declaration,
				MultiLODRenderData->LODVertexFactories[1].VertexFactory.GetDeclaration());

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
		Durin::FPrimitiveComponentId(101),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}),
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
					CommandList, MultiLODScene.GetPrimitiveSceneInfos(), View,
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
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(MultiLODScene, Durin::FPrimitiveComponentId(101));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner DegenerateTransformSceneOwner;

	Durin::FScene& DegenerateTransformScene = *DegenerateTransformSceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(DegenerateTransformScene,
		Durin::FPrimitiveComponentId(102),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}),
		Durin::Math::ScaleMatrix(Durin::FVector3(0.0, 1.0, 1.0)));
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(DegenerateTransformScene,
		Durin::FPrimitiveComponentId(103),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}),
		Durin::Math::ScaleMatrix(Durin::FVector3(1.0e-12, 1.0, 1.0)));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&DegenerateTransformScene](
			Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList,
					DegenerateTransformScene.GetPrimitiveSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			EXPECT_TRUE(Prepared.Primitives.empty());
			EXPECT_EQ(Prepared.GetNumSections(), 0u);
			EXPECT_EQ(Prepared.RejectedPrimitives, 2u);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(DegenerateTransformScene, Durin::FPrimitiveComponentId(102));
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(DegenerateTransformScene, Durin::FPrimitiveComponentId(103));
	Durin::FlushRenderingCommands();

	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(RenderData.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Replaced =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(), Durin::FSceneView{},
					Durin::ERasterMode::Solid
				);
			ASSERT_EQ(Replaced.GetNumSections(), 4u);
			EXPECT_EQ(Replaced.Opaque.front().Command->PipelineKey.Rasterizer.FrontFace, Durin::ERHIFrontFace::Clockwise);
		}
	);
	Durin::FlushRenderingCommands();

	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(Scene, Id);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			EXPECT_EQ(Durin::PrepareStaticMeshView_RenderThread(
				CommandList, Scene.GetPrimitiveSceneInfos(), Durin::FSceneView{},
				Durin::ERasterMode::Solid).GetNumSections(), 0u);
			auto RetainedGeometry = RenderData->LODResources[0].GeometryRecord;
			ASSERT_TRUE(RetainedGeometry);
			std::shared_ptr<const Durin::FCollectedStaticMeshView> Collected;
			Durin::FPreparedStaticMeshView RetainedFrame;
			{
				const auto RetainedProxy = std::make_shared<Durin::FStaticMeshSceneProxy>(RenderData.get(),
					std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent, Opaque});
				RetainedProxy->SetFeatureLevel(Scene.GetFeatureLevel());
				auto RetainedInfo = std::make_unique<Durin::FPrimitiveSceneInfo>(Durin::FPrimitiveComponentId(710), RetainedProxy, Durin::FMatrix(1.0), &Scene);
				Scene.GPUScene->AddPrimitive(*RetainedInfo);
				const std::array<const Durin::FPrimitiveSceneInfo*, 1> RetainedInfos{RetainedInfo.get()};
				Durin::FSceneView SourceView;
				Collected = Durin::CollectStaticMeshView_RenderThread(CommandList, RetainedInfos,
					SourceView, Durin::ERasterMode::Solid);
				RetainedFrame = Durin::PrepareStaticMeshView_RenderThread(CommandList, RetainedInfos,
					SourceView, Durin::ERasterMode::Solid);
				SourceView.ViewLocation = Durin::FVector3(500.0);
				SourceView.DepthConvention = Durin::ESceneDepthConvention::ReversedZ;
				Scene.GPUScene->RemovePrimitive(*RetainedInfo);
			}
			ASSERT_EQ(RetainedFrame.GetNumSections(), 4u);
			EXPECT_EQ(RetainedFrame.Primitives[0].GeometryRecord, RetainedGeometry);
			std::weak_ptr<const Durin::FMeshGeometryRecord> RetiredGeometry = RetainedGeometry;
			const auto OldRecordId = RetainedGeometry->GetRecordId();
			RenderData->ReleaseResources();
			EXPECT_FALSE(RenderData->LODResources[0].GeometryRecord);
			EXPECT_TRUE(RetainedGeometry->GetElements()[0].Vertices.IsValid());
			EXPECT_TRUE(RetainedGeometry->GetElements()[0].Indices.IsValid());
			EXPECT_TRUE(RetainedGeometry->GetBinding()->Declaration);
			ASSERT_TRUE(RenderData->InitResources(CommandList));
			EXPECT_NE(RenderData->LODResources[0].GeometryRecord->GetRecordId(), OldRecordId);
			EXPECT_NE(RenderData->LODResources[0].GeometryRecord->GetBinding(), RetainedGeometry->GetBinding());
			EXPECT_EQ(RetainedGeometry->GetRecordId(), OldRecordId);
			// Scene info/proxy and source view are gone, and asset resources have a
			// new publication. Replaying the captured collection still uses old facts.
			auto Replay = Durin::PrepareCollectedStaticMeshView_RenderThread(CommandList, Collected);
			// Source removal invalidates preparation; captured CPU facts are not a historical render submission.
			EXPECT_EQ(Replay.GetNumSections(), 0u);
			RetainedGeometry.reset();
			EXPECT_FALSE(RetiredGeometry.expired());
			EXPECT_TRUE(RetainedFrame.Opaque[0].Command->Vertices.IsValid());
			EXPECT_EQ(RetainedFrame.Primitives[0].GeometryRecord->GetRecordId(), OldRecordId);
			RetainedFrame = {};
			Replay = {};
			EXPECT_FALSE(RetiredGeometry.expired());
			Collected.reset();
			EXPECT_TRUE(RetiredGeometry.expired());
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
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}

TEST(FStaticMeshRenderPreparationVulkanTests,
	PreparesMixedGeometryAcrossRepeatedViews)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	InitializeDObjectSystem();
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	auto RenderData = MakeRenderData();
	const std::vector<Durin::FMaterialRenderProxyRef> Materials{
		MakeMaterial(Durin::EMaterialBlendMode::Opaque),
		MakeMaterial(Durin::EMaterialBlendMode::Masked),
		MakeMaterial(Durin::EMaterialBlendMode::Translucent),
		MakeMaterial(Durin::EMaterialBlendMode::Opaque, true)};
	Durin::FSceneTestOwner SceneOwner;
	auto& Scene = *SceneOwner;
	Durin::EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			ASSERT_TRUE(RenderData->InitResources(CommandList));
		});
	Durin::FlushRenderingCommands();

	constexpr size_t PrimitiveCount = 4;
	for (size_t Index = 0; Index < PrimitiveCount; ++Index)
	{
		std::unique_ptr<Durin::FPrimitiveSceneProxy> Proxy;
		if (Index % 2 == 0)
		{
			Proxy = std::make_unique<Durin::FStaticMeshSceneProxy>(
				RenderData.get(), Materials);
		}
		else
		{
			Durin::FSplineMeshRenderDynamicData Dynamic{
				.Params = {},
				.LocalBounds = Durin::FBox({-2.0, -2.0, -1.0}, {120.0, 40.0, 20.0})};
			Dynamic.Params.EndPosition = {100.0, 30.0, 10.0};
			Dynamic.Params.EndTangent = {80.0, 0.0, 10.0};
			Dynamic.Params.SourceForwardMin = -1.0;
			Dynamic.Params.SourceForwardMax = 1.0;
			Proxy = std::make_unique<Durin::FSplineMeshSceneProxy>(
				RenderData.get(), Materials, Dynamic);
		}
		Durin::FSceneInterfaceTestAccess::AddPrimitiveProxy(
			Scene, Durin::FPrimitiveComponentId(1000 + Index), std::move(Proxy),
			Durin::FMatrix(1.0));
	}
	Durin::FlushRenderingCommands();

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			for (size_t Frame = 0; Frame < 2; ++Frame)
			{
				const auto Prepared = Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(), Durin::FSceneView{},
					Durin::ERasterMode::Solid);
				ASSERT_EQ(Prepared.Primitives.size(), PrimitiveCount);
				EXPECT_EQ(Prepared.Opaque.size(), 8u);
				EXPECT_EQ(Prepared.Masked.size(), 4u);
				EXPECT_EQ(Prepared.Translucent.size(), 4u);
				EXPECT_EQ(Prepared.RejectedPrimitives, 0u);
				EXPECT_EQ(Prepared.PreparedSplinePrimitives, 2u);
			}
		});
	Durin::FlushRenderingCommands();
	SceneOwner.Reset();
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>(
		[&](Durin::FRHICommandListImmediate&) { RenderData->ReleaseResources(); });
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::RHIExit();
}

namespace Durin::Tests
{
	DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FQualificationVertexShader);
	DURIN_IMPLEMENT_GLOBAL_SHADER(FDepthCaptureVertexShader);
	DURIN_IMPLEMENT_GLOBAL_SHADER(FDepthCaptureFragmentShader);
	DURIN_IMPLEMENT_GLOBAL_SHADER(FShadowCaptureFragmentShader);
}

namespace Durin::Tests
{
	// Own shader setup and terminal compiler shutdown for the entire target.
	class FGeometryQualificationEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			const auto Root = Testing::CreateTestFixtureDirectory("GeometryQualification");
			std::filesystem::create_directories(Root / "Source");
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteView(
				reinterpret_cast<const std::byte*>(QualificationVertexSource.data()), QualificationVertexSource.size()), Root / "Source/Vertex.slang"));
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::span(reinterpret_cast<const std::byte*>(DepthCaptureSource.data()), DepthCaptureSource.size()), Root / "Source/Depth.slang"));
			FShaderPaths::RegisterMountPoint("/GeometryQualification/", (Root / "Source").generic_string(), (Root / "Cache").generic_string());
			ASSERT_TRUE(RendererPrivate::RegisterMeshVertexFactory(std::make_shared<FQualificationFactory>()));
			ASSERT_TRUE(RendererPrivate::RegisterMeshVertexFactory(std::make_shared<FQualificationFactory>(true)));
			FModuleManager::Get().LoadModuleChecked("ShaderBuild");
			ASSERT_TRUE(PrepareGeometryDepthCaptureShadersSync());
		}
		auto TearDown() -> void override
		{
			// Per-case shutdown would make later cases attempt a forbidden restart.
			GeometryDepthCaptureOutputs = {};
			ShutdownAssetCompilingManager();
			FModuleManager::Get().ShutdownModule("ShaderBuild");
		}
	};
	[[maybe_unused]] const auto GeometryEnvironment = testing::AddGlobalTestEnvironment(new FGeometryQualificationEnvironment());
}

namespace
{
	// Every preparation pump owns a complete frame, including early Pending exits.
	class FGeometryQualificationFrame final
	{
	public:
		explicit FGeometryQualificationFrame(Durin::FRHICommandListImmediate& InCommands) : Commands(InCommands)
		{
			++Durin::GRenderFrameCounterRenderThread;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
		}
		~FGeometryQualificationFrame()
		{
			Durin::SetGBufferCaptureSink(nullptr);
			Durin::SetShadowDepthCaptureSink(nullptr);
			Durin::SetViewRenderTelemetrySink(nullptr);
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(Commands);
		}
	private:
		Durin::FRHICommandListImmediate& Commands;
	};

	class FGeometryRendererCleanup final
	{
	public:
		explicit FGeometryRendererCleanup(Durin::FModuleTestHarness& InLifecycle) : Lifecycle(InLifecycle) {}
		~FGeometryRendererCleanup()
		{
			Durin::FlushRenderingCommands();
			Lifecycle.Shutdown();
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
			Durin::RHIExit();
		}
	private:
		Durin::FModuleTestHarness& Lifecycle;
	};

	auto RenderPreparedGeometryView(Durin::FRendererModule& Renderer, Durin::FSceneInterface& Scene,
		const Durin::FSceneView& View, const Durin::FRHITextureCreateDesc& TargetDesc,
		const std::function<void()>& BeforeDraw = {},
		const std::function<void(Durin::FRHICommandListImmediate&, Durin::FRHITexture*)>& AfterDraw = {},
		const Durin::FSceneViewRenderOptions& Options = {},
		std::optional<Durin::EGPUCullingFailure> CullingFailure = {},
		uint64* PreparationNanoseconds = nullptr, uint64* RecordingNanoseconds = nullptr) -> Durin::ERenderViewResult
	{
		using namespace Durin;
		if (PreparationNanoseconds) *PreparationNanoseconds = 0;
		if (RecordingNanoseconds) *RecordingNanoseconds = 0;
		const auto Preparation = FRenderResourcePreparation::Create();
		FTextureRHIRef Target;
		auto Result = ERenderViewResult::RendererResourcesPending;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		do
		{
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
				FGeometryQualificationFrame Frame(Commands);
				auto Policy = GetRendererQualificationPolicy();
				if (CullingFailure) Policy.GPUCullingFailure = *CullingFailure;
				FScopedRendererQualificationPolicy Qualification(Policy);
				if (!Target) Target = GDynamicRHI->RHICreateTexture(Commands, TargetDesc);
				if (!Target) { Result = ERenderViewResult::RendererResourcesUnavailable; return; }
				const auto DrawsBefore = Commands.GetNumRecordedDrawCommands();
                const auto PreparationStart = std::chrono::steady_clock::now();
				Renderer.PrepareViewResources(Commands, {&Scene, View, Target, false, Options}, Preparation);
                if (PreparationNanoseconds) *PreparationNanoseconds += static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - PreparationStart).count());
				EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), DrawsBefore);
				const auto State = Preparation.GetProgress().State;
				if (State == ERenderResourcePreparationState::Pending) return;
				if (State != ERenderResourcePreparationState::Ready)
				{ Result = ERenderViewResult::RendererResourcesUnavailable; return; }
				if (BeforeDraw) BeforeDraw();
				if (testing::Test::HasFatalFailure())
				{ Result = ERenderViewResult::RendererResourcesUnavailable; return; }
				const auto RecordingStart = std::chrono::steady_clock::now();
				Result = Renderer.RenderView(Commands,
					{&Scene, View, Target, false, Options});
				if (RecordingNanoseconds) *RecordingNanoseconds += static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - RecordingStart).count());
				ASSERT_NE(Result, ERenderViewResult::RendererResourcesPending)
					<< "A ready view must finish recording even when capture exceeds the preparation time budget.";
				if (Result == ERenderViewResult::Success)
					EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Ready);
				if (Result == ERenderViewResult::Success && AfterDraw) AfterDraw(Commands, Target);
			});
			FlushRenderingCommands();
			if (Result != ERenderViewResult::RendererResourcesPending || testing::Test::HasFatalFailure()) break;
			// Only the host yields between frames; the render thread never joins Shader work.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < Deadline);
		if (Result == ERenderViewResult::RendererResourcesPending) Preparation.RequestCancel();
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) {
			if (Result == ERenderViewResult::RendererResourcesPending)
				Preparation.Retire_RenderThread(ERenderResourcePreparationState::Canceled);
			Target = {};
		});
		FlushRenderingCommands();
		return Result;
	}
}

TEST(FStaticMeshRenderPreparationVulkanTests, DepthReadbackConsumesHostPreparedShaders)
{
	using namespace Durin;
	using namespace Durin::Tests;
	ASSERT_EQ(GDynamicRHI, nullptr);
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([](FRHICommandListImmediate& Commands) {
		++GRenderFrameCounterRenderThread;
		GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
		for (bool Array : {false, true})
		{
			auto Desc = Array ? FRHITextureCreateDesc::Create2DArray("PreparedShadowInput")
				: FRHITextureCreateDesc::Create2D("PreparedDepthInput");
			Desc.SetExtent(8, 8).SetFormat(EPixelFormat::R32_FLOAT).SetArraySize(Array ? 2 : 1)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
			const auto Source = GDynamicRHI->RHICreateTexture(Commands, Desc);
			auto ViewDesc = MakeDefaultTextureViewDesc(*Source, ERHITextureViewUsage::ColorAttachment);
			ViewDesc.Range.NumArrayLayers = 1;
			const auto View = GDynamicRHI->RHICreateTextureView(Source, ViewDesc);
			FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout = RenderTargetLayouts::MakeGBufferDebugOutput();
			Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::R32_FLOAT;
			Pass.ColorRenderTargets[0] = Source;
			Pass.ColorRenderTargetViews[0] = View;
			Pass.ColorClearValues[0] = FClearValueBinding(0.25f, 0.0f, 0.0f);
			Commands.BeginRenderPass(Pass, "PreparedDepthInput");
			Commands.EndRenderPass();
			FByteBuffer Pixels;
			{
				FPreparedShaderMapScope Capture;
				ReadGeometryTexture(Commands, Source, Pixels, true);
			}
			EXPECT_EQ(Pixels.size(), 8u * 8u * sizeof(float));
			for (size_t Offset = 0; Offset + sizeof(float) <= Pixels.size(); Offset += sizeof(float))
			{
				float Value;
				std::memcpy(&Value, Pixels.data() + Offset, sizeof(Value));
				EXPECT_FLOAT_EQ(Value, 0.25f);
			}
		}
		GDynamicRHI->RHIEndFrame_RenderThread(Commands);
	});
	FlushRenderingCommands();
	ShutdownRenderingThread();
	FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
	RHIExit();
}

TEST(FStaticMeshRenderPreparationVulkanTests, QualifiesIndependentMultiBatchGeometryAndFactory)
{
	using namespace Durin;
	using namespace Durin::Tests;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId();
	GIsGameThreadIdInitialized = true;
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");

	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness RendererLifecycle("GeometryQualification");
	RendererLifecycle.Start(Renderer);
	struct FRendererCleanup
	{
		FModuleTestHarness& Lifecycle;
		~FRendererCleanup()
		{
			FlushRenderingCommands();
			Lifecycle.Shutdown();
			ShutdownRenderingThread();
			FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
			RHIExit();
		}
	} Cleanup{RendererLifecycle};
	FSceneTestOwner Owner;
	auto& Scene = *Owner;
	const auto Opaque = MakeMaterial(EMaterialBlendMode::Opaque, true);
	const auto Masked = MakeMaterial(EMaterialBlendMode::Masked, true);
	std::array<FProceduralGeometry, 2> Geometries;
	std::array<FMaterialRenderData, 2> Materials;
	std::shared_ptr<const FStaticMeshPreparationInputs> FrozenInput;
	FPreparedStaticMeshView FrozenExpected;
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& CommandList) {
		Materials = {Opaque->GetMaterialWithFallback(), Masked->GetMaterialWithFallback()};
		// Keep the entry-point collision that previously substituted the
		// generated Local vertex stage for this independent factory's shader.
		for (const auto& Material : Materials)
		{
			ASSERT_TRUE(Material.CompiledProgram);
			ASSERT_TRUE(std::ranges::any_of(Material.CompiledProgram->GetShaderMap().CompiledShaders,
				[](const FShaderCode& Shader) {
					return Shader.Frequency == EShaderFrequency::Vertex
						&& Shader.SourceEntryPoint == FQualificationVertexShader::StaticType().GetEntryPoint();
				}));
		}
		Geometries[0] = FProceduralGeometry::Create(CommandList, 1, false);
		Geometries[1] = FProceduralGeometry::Create(CommandList, 2, true);
		Geometries[1].Binding->DisplacementScale = {0.05f, 0.0f, 0.0f, 0.8f};
	});
	FlushRenderingCommands();
	for (size_t I = 0; I < Geometries.size(); ++I)
	{
		auto Proxy = std::make_unique<FProceduralProxy>();
		Proxy->Geometry = Geometries[I]; Proxy->Materials = Materials; Proxy->bReverse = I == 1;
		FSceneInterfaceTestAccess::AddPrimitiveProxy(Scene, FPrimitiveComponentId(100 + I), std::move(Proxy), FMatrix(1.0));
	}
	FlushRenderingCommands();
	FPreparedStaticMeshView Snapshot;
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& CommandList) {
		FSceneView View;
		View.ViewportWidth = 192; View.ViewportHeight = 108;
		View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		FViewRenderTelemetry Telemetry;
		const auto Visible = PrepareSceneVisibility(Scene, View, Telemetry);
		ASSERT_EQ(Visible.SceneInfos.size(), 2u);
		Snapshot = PrepareStaticMeshView_RenderThread(CommandList, Visible.SceneInfos, View, ERasterMode::Solid);
		ASSERT_EQ(Snapshot.Primitives.size(), 4u);
		EXPECT_EQ(Snapshot.PublishedGeometryElements, 0u);
		EXPECT_EQ(Snapshot.DynamicGeometryInputValidations, 4u);
		EXPECT_EQ(Snapshot.VisibleCandidates, 2u);
		EXPECT_EQ(Snapshot.PreparedLocalPrimitives, 2u);
		EXPECT_EQ(Snapshot.RejectedPrimitives, 0u);
		ASSERT_EQ(Snapshot.GetNumSections(), 4u);
		EXPECT_EQ(Snapshot.Opaque.size(), 2u);
		EXPECT_EQ(Snapshot.Masked.size(), 2u);
		EXPECT_EQ(Snapshot.Opaque[0].MaterialUniformIndex, Snapshot.Opaque[1].MaterialUniformIndex);
		EXPECT_EQ(Snapshot.Masked[0].MaterialUniformIndex, Snapshot.Masked[1].MaterialUniformIndex);
		for (uint32 Group = 0; Group < Snapshot.MaterialUniformGroups.size(); ++Group)
			EXPECT_EQ(Snapshot.GetDraw(Snapshot.MaterialUniformGroups[Group].RepresentativeDraw).MaterialUniformIndex, Group);
		EXPECT_GE(Snapshot.Opaque.front().Command->SectionIndex, uint64{1} << 48);
		EXPECT_GE(Snapshot.Primitives.front().BatchId, uint64{1} << 40);
		EXPECT_EQ(Snapshot.Masked.front().Command->Geometry.FirstElement, 3u);
		EXPECT_EQ(Snapshot.Masked.front().Command->Geometry.FirstInstance, 2u);
		EXPECT_EQ(Snapshot.Masked.front().Command->Geometry.InstanceCount, 2u);
		// An independent factory opts into the same persistent path without an asset.
		auto PublishedProxy = std::make_shared<FProceduralProxy>();
		PublishedProxy->SetFeatureLevel(Scene.GetFeatureLevel());
		PublishedProxy->Geometry = Geometries[1];
		PublishedProxy->Materials = Materials;
		PublishedProxy->bReverse = true;
		ASSERT_TRUE(PublishedProxy->PublishGeometry());
		FPrimitiveSceneInfo PublishedInfo(FPrimitiveComponentId(101), PublishedProxy, FMatrix(1.0));
		const std::array<const FPrimitiveSceneInfo*, 1> PublishedInfos{&PublishedInfo};
		const auto CapturedPublished = CollectStaticMeshView_RenderThread(CommandList, PublishedInfos, View, ERasterMode::Solid);
		const auto QueriesBeforeReplay = FQualificationFactory::SupportQueries.load(std::memory_order_relaxed);
		FStaticMeshPreparationCache FrozenCache;
		FMeshDrawDescriptionCache TemplateCache;
		TemplateCache.BeginSubmission();
		FrozenCache.Commands = &TemplateCache;
		FrozenInput = ResolveCollectedStaticMeshView_RenderThread(CommandList, CapturedPublished, &FrozenCache);
		const auto PublishedView = PrepareStaticMeshInputs(*FrozenInput);
		FrozenExpected = PublishedView;
		EXPECT_EQ(FQualificationFactory::SupportQueries.load(std::memory_order_relaxed), QueriesBeforeReplay);
		ASSERT_GT(QueriesBeforeReplay, 0u);
		FrozenInput = ResolveCollectedStaticMeshView_RenderThread(CommandList,
			CollectStaticMeshView_RenderThread(CommandList, Visible.SceneInfos, View, ERasterMode::Solid), &FrozenCache);
		FrozenExpected = PrepareStaticMeshInputs(*FrozenInput);
		ASSERT_TRUE(IsTaskSchedulerRunning());
		std::vector<FMeshViewPreparationInput> LODInputs;
		for (size_t Index = 0; Index < 1536; ++Index)
			LODInputs.push_back({Index % 5 == 0 ? FBox{} : Visible.SceneInfos.front()->GetWorldBounds(),
				Index % 3 == 0 ? std::optional<FMeshLODSelectionSnapshot>{}
					: FMeshLODSelectionSnapshot{{0.5f, false}, {0.25f, Index % 2 == 0}, {0.0f, true}}});
		for (auto Mode : {EViewLODMode::ForceLOD0, View.Settings.Mode.LODMode})
		{
			auto LODView = View;
			LODView.Settings.Mode.LODMode = Mode;
			const auto SerialFacts = PrepareMeshViewFacts(LODInputs, LODView, false);
			const auto WorkerFacts = PrepareMeshViewFacts(LODInputs, LODView);
			EXPECT_EQ(WorkerFacts.TaskCount, 12u);
			ASSERT_EQ(WorkerFacts.Primitives.size(), SerialFacts.Primitives.size());
			for (size_t Index = 0; Index < WorkerFacts.Primitives.size(); ++Index)
			{
				const auto& Actual = WorkerFacts.Primitives[Index];
				const auto& Expected = SerialFacts.Primitives[Index];
				EXPECT_EQ(Actual.Projected.Status, Expected.Projected.Status);
				EXPECT_FLOAT_EQ(Actual.Projected.NormalizedScreenSize, Expected.Projected.NormalizedScreenSize);
				ASSERT_EQ(Actual.LOD.has_value(), Expected.LOD.has_value());
				if (Actual.LOD)
				{
					EXPECT_EQ(Actual.LOD->Requested, Expected.LOD->Requested);
					EXPECT_EQ(Actual.LOD->Selected, Expected.LOD->Selected);
					if (Mode == EViewLODMode::ForceLOD0) EXPECT_EQ(Actual.LOD->Requested, 0u);
				}
			}
		}
		std::vector<FPrimitiveVisibilityInput> VisibilityInputs;
		for (size_t Index = 0; Index < 6144; ++Index)
		{
			const auto Bounds = Index % 3 == 0 ? FBox{}
				: Index % 3 == 1 ? FBox(FVector3(1e9), FVector3(1e9 + 1))
				: Visible.SceneInfos.front()->GetWorldBounds();
			VisibilityInputs.push_back({Bounds, Index % 4 != 0});
		}
		auto VisibilityView = View;
		VisibilityView.Settings.Mode.VisibilityMode = EViewVisibilityMode::Normal;
		const auto SerialVisibility = ClassifySceneVisibility(VisibilityInputs, VisibilityView, false);
		const auto TaskVisibility = ClassifySceneVisibility(VisibilityInputs, VisibilityView);
		EXPECT_EQ(TaskVisibility.TaskCount, 12u);
		EXPECT_EQ(TaskVisibility.Primitives, SerialVisibility.Primitives);
		EXPECT_EQ(TaskVisibility.Primitives.front(), EPrimitiveVisibilityClassification::Hidden);
		EXPECT_EQ(TaskVisibility.Primitives[3], EPrimitiveVisibilityClassification::VisibleInvalidBoundsFallback);
		auto InvalidVisibilityView = VisibilityView;
		InvalidVisibilityView.ViewProjectionMatrix = FMatrix(0.0);
		const auto InvalidVisibility = ClassifySceneVisibility(VisibilityInputs, InvalidVisibilityView);
		EXPECT_EQ(InvalidVisibility.Primitives, ClassifySceneVisibility(VisibilityInputs, InvalidVisibilityView, false).Primitives);
		EXPECT_EQ(InvalidVisibility.Primitives[1], EPrimitiveVisibilityClassification::VisibleInvalidViewFallback);
		EXPECT_EQ(ClassifySceneVisibility({}, View).TaskCount, 0u);
		auto Small = FinishStaticMeshPreparation(StartStaticMeshPreparation(FrozenInput));
		ASSERT_TRUE(Small);
		EXPECT_EQ(Small->PreparationTaskCount, 0u);
		auto LargeCollected = std::make_shared<FCollectedStaticMeshView>(*FrozenInput->Collected);
		auto LargeInputs = std::make_shared<FStaticMeshPreparationInputs>(*FrozenInput);
		LargeCollected->Primitives.clear();
		LargeInputs->Primitives.clear();
		for (size_t Index = 0; Index < 1536; ++Index)
		{
			const size_t Source = Index % FrozenInput->Primitives.size();
			LargeCollected->Primitives.push_back(FrozenInput->Collected->Primitives[Source]);
			LargeCollected->Primitives.back().Id = FPrimitiveComponentId(10000 + Index);
			LargeInputs->Primitives.push_back(FrozenInput->Primitives[Source]);
		}
		LargeInputs->Collected = LargeCollected;
		const auto LargeExpected = PrepareStaticMeshInputs(*LargeInputs);
		auto Work = StartStaticMeshPreparation(LargeInputs);
		EXPECT_EQ(Work.Active.size(), 8u);
		EXPECT_EQ(Work.LaunchedChunks, 8u);
		auto Parallel = FinishStaticMeshPreparation(std::move(Work));
		ASSERT_TRUE(Parallel);
		EXPECT_EQ(Parallel->PreparationTaskCount, 12u);
		ASSERT_EQ(Parallel->GetNumSections(), LargeExpected.GetNumSections());
		EXPECT_EQ(Parallel->SubmissionOutcomes, LargeExpected.SubmissionOutcomes);
		EXPECT_EQ(Parallel->SelectedTriangles, LargeExpected.SelectedTriangles);
		EXPECT_EQ(Parallel->RequestedLODHistogram, LargeExpected.RequestedLODHistogram);
		EXPECT_EQ(Parallel->SelectedLODHistogram, LargeExpected.SelectedLODHistogram);
		EXPECT_EQ(Parallel->PipelineTransitions, LargeExpected.PipelineTransitions);
		for (uint32 Index = 0; Index < Parallel->GetNumSections(); ++Index)
		{
			EXPECT_EQ(Parallel->GetDraw(Index).SortKey, LargeExpected.GetDraw(Index).SortKey);
			EXPECT_EQ(Parallel->GetDraw(Index).PrimitiveIndex, LargeExpected.GetDraw(Index).PrimitiveIndex);
			EXPECT_EQ(Parallel->GetDraw(Index).ResolvedIndex, LargeExpected.GetDraw(Index).ResolvedIndex);
			EXPECT_EQ(Parallel->GetDraw(Index).MaterialUniformIndex, LargeExpected.GetDraw(Index).MaterialUniformIndex);
		}
		Tasks::FCancellationToken Cancellation;
		Cancellation.Cancel();
		for (auto Policy : {EStaticMeshPreparationPolicy::Inline, EStaticMeshPreparationPolicy::Tasks})
		{
			auto Canceled = FinishStaticMeshPreparation(StartStaticMeshPreparation(LargeInputs, Policy, [&Cancellation] { return Cancellation.IsCanceled(); }));
			ASSERT_FALSE(Canceled);
			EXPECT_EQ(Canceled.error().State, ETaskState::Canceled);
		}
		FStaticMeshPreparationWork FailedWork;
		FailedWork.Inputs = FrozenInput;
		FailedWork.NextPrimitive = FrozenInput->Primitives.size();
		FailedWork.Active.push_back({0, Tasks::LaunchIndependentTask("MeshPreparationFailure", []() -> FStaticMeshPreparationChunk {
			throw std::runtime_error("Injected preparation failure");
		})});
		auto Failed = FinishStaticMeshPreparation(std::move(FailedWork));
		ASSERT_FALSE(Failed);
		EXPECT_EQ(Failed.error().State, ETaskState::Failed);
		EXPECT_EQ(Failed.error().ChunkIndex, 0u);
		std::vector<Durin::Tasks::FTask> AbandonedCompletions;
		{
			auto Abandoned = StartStaticMeshPreparation(LargeInputs, EStaticMeshPreparationPolicy::Tasks);
			for (const auto& Slot : Abandoned.Active) AbandonedCompletions.push_back(Durin::Tasks::FTask(Slot.Task));
		}
		ASSERT_EQ(AbandonedCompletions.size(), 8u);
		for (const auto& Completion : AbandonedCompletions)
		{
			EXPECT_TRUE(Completion.IsCompleted());
			EXPECT_TRUE(Completion.GetState() == ETaskState::Succeeded || Completion.GetState() == ETaskState::Canceled);
		}
		ASSERT_EQ(PublishedView.GetNumSections(), 2u);
		EXPECT_EQ(PublishedView.DynamicGeometryInputValidations, 0u);
		EXPECT_EQ(PublishedView.PublishedGeometryElements, 2u);
		for (uint32 I = 0; I < PublishedView.GetNumSections(); ++I)
		{
			const auto& Actual = PublishedView.GetDraw(I);
			const auto& Family = Actual.Command->Pass == EMeshBasePass::Opaque ? Snapshot.Opaque : Snapshot.Masked;
			const auto Expected = std::ranges::find_if(Family, [&](const auto& Draw) {
				return Snapshot.Primitives[Draw.PrimitiveIndex].PrimitiveId == FPrimitiveComponentId(101);
			});
			ASSERT_NE(Expected, Family.end());
			EXPECT_EQ(Actual.SortKey, Expected->SortKey);
			EXPECT_EQ(Actual.Command->PipelineKey, Expected->Command->PipelineKey);
			EXPECT_EQ(Actual.Command->Geometry.FirstInstance, Expected->Command->Geometry.FirstInstance);
			EXPECT_EQ(Actual.Command->Vertices.Buffer, Expected->Command->Vertices.Buffer);
		}
		const auto PublishedShadow = PrepareStaticMeshView_RenderThread(CommandList, PublishedInfos, View,
			ERasterMode::Solid, ERenderPreparationMode::ShadowDepth);
		EXPECT_EQ(PublishedShadow.GetNumSections(), 1u);
		EXPECT_EQ(PublishedShadow.DynamicGeometryInputValidations, 0u);
		const auto Shadow = PrepareStaticMeshView_RenderThread(CommandList, Visible.SceneInfos, View, ERasterMode::Solid, ERenderPreparationMode::ShadowDepth);
		EXPECT_EQ(Shadow.GetNumSections(), 2u);
		EXPECT_EQ(Shadow.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Excluded)], 2u);
		auto Invalid = *Geometries[0].Binding;
		Invalid.Streams[0].Offset = Invalid.Streams[0].VertexBuffer->GetSize();
		EXPECT_EQ(Invalid.ValidateInputs(Snapshot.Opaque.front().Command->Geometry), EGeometrySubmissionOutcome::InvalidSubmission);
		Invalid = *Geometries[0].Binding;
		Invalid.DeclarationElements[0].Stride = 1;
		EXPECT_EQ(Invalid.ValidateInputs(Snapshot.Opaque.front().Command->Geometry), EGeometrySubmissionOutcome::InvalidSubmission);
		const auto MissingProxy = std::make_shared<FProceduralProxy>();
		MissingProxy->SetFeatureLevel(Scene.GetFeatureLevel());
		MissingProxy->Geometry = Geometries[0];
		MissingProxy->Geometry.Binding = std::make_shared<FProceduralBinding>(*Geometries[0].Binding);
		MissingProxy->Geometry.Binding->Streams.clear();
		MissingProxy->Materials = Materials;
		FPrimitiveSceneInfo MissingInfo(FPrimitiveComponentId(900), MissingProxy, FMatrix(1.0));
		const std::array<FPrimitiveSceneInfo*, 1> MissingInfos{&MissingInfo};
		const auto Missing = PrepareStaticMeshView_RenderThread(CommandList, MissingInfos, View, ERasterMode::Solid);
		EXPECT_TRUE(Missing.bResourceFailure);
		EXPECT_EQ(Missing.GetNumSections(), 0u);
		EXPECT_EQ(Missing.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)], 2u);
	});
	FlushRenderingCommands();
	FSceneView InitialView;
	InitialView.ViewportWidth = 192; InitialView.ViewportHeight = 108;
	InitialView.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
	for (auto Mode : {ERenderMode::Unlit, ERenderMode::Lit})
	{
		InitialView.Settings.Mode.RenderMode = Mode;
		ASSERT_EQ(RenderPreparedGeometryView(Renderer, Scene, InitialView,
			FRHITextureCreateDesc::Create2D("IndependentGeometryColor", 192, 108, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)),
			ERenderViewResult::Success);
	}

	ASSERT_TRUE(FrozenInput);
	{
		// Both render-thread preparation caches are already gone.
		// Independent worker results own their scratch and share only const inputs.
		std::array<FPreparedStaticMeshView, 3> WorkerResults;
		std::array<std::jthread, 2> Workers;
		for (size_t Index = 0; Index < Workers.size(); ++Index)
			Workers[Index] = std::jthread([&, Index] { WorkerResults[Index] = PrepareStaticMeshInputs(*FrozenInput); });
		for (auto& Worker : Workers) Worker.join();
		ASSERT_GE(FrozenInput->Collected->Primitives.size(), 2u);
		std::vector<FStaticMeshPreparationChunk> Chunks(2);
		// Arrival order is deliberately the opposite of primitive order.
		Workers[0] = std::jthread([&] { Chunks[0] = PrepareStaticMeshInputChunk(FrozenInput, 1,
			FrozenInput->Collected->Primitives.size() - 1); });
		Workers[1] = std::jthread([&] { Chunks[1] = PrepareStaticMeshInputChunk(FrozenInput, 0, 1); });
		for (auto& Worker : Workers) Worker.join();
		auto Merged = MergeStaticMeshPreparationChunks(std::move(Chunks));
		ASSERT_TRUE(Merged);
		WorkerResults[2] = std::move(*Merged);
		auto FirstChunk = PrepareStaticMeshInputChunk(FrozenInput, 0, 1);
		const auto Missing = MergeStaticMeshPreparationChunks({FirstChunk});
		ASSERT_FALSE(Missing);
		EXPECT_EQ(Missing.error(), EStaticMeshPreparationMergeError::IncompleteCoverage);
		const auto Duplicate = MergeStaticMeshPreparationChunks({FirstChunk, FirstChunk});
		ASSERT_FALSE(Duplicate);
		EXPECT_EQ(Duplicate.error(), EStaticMeshPreparationMergeError::IncompleteCoverage);
		FirstChunk.Inputs = std::make_shared<FStaticMeshPreparationInputs>(*FrozenInput);
		const auto Mismatched = MergeStaticMeshPreparationChunks({FirstChunk, PrepareStaticMeshInputChunk(FrozenInput, 1, 1)});
		ASSERT_FALSE(Mismatched);
		EXPECT_EQ(Mismatched.error(), EStaticMeshPreparationMergeError::MismatchedInputs);
		const auto NoChunks = MergeStaticMeshPreparationChunks({});
		ASSERT_FALSE(NoChunks);
		EXPECT_EQ(NoChunks.error(), EStaticMeshPreparationMergeError::EmptyChunks);
		auto EmptyInputs = std::make_shared<FStaticMeshPreparationInputs>();
		EmptyInputs->Collected = std::make_shared<const FCollectedStaticMeshView>();
		const auto Empty = MergeStaticMeshPreparationChunks({PrepareStaticMeshInputChunk(EmptyInputs, 0, 0)});
		ASSERT_TRUE(Empty);
		EXPECT_EQ(Empty->GetNumSections(), 0u);
		for (const auto& Actual : WorkerResults)
		{
			ASSERT_EQ(Actual.GetNumSections(), FrozenExpected.GetNumSections());
			EXPECT_EQ(Actual.SubmissionOutcomes, FrozenExpected.SubmissionOutcomes);
			EXPECT_EQ(Actual.SelectedTriangles, FrozenExpected.SelectedTriangles);
			EXPECT_EQ(Actual.DrawDescriptionBuilds, FrozenExpected.DrawDescriptionBuilds);
			EXPECT_EQ(Actual.VisibleCandidates, FrozenExpected.VisibleCandidates);
			EXPECT_EQ(Actual.RequestedLODHistogram, FrozenExpected.RequestedLODHistogram);
			EXPECT_EQ(Actual.SelectedLODHistogram, FrozenExpected.SelectedLODHistogram);
			EXPECT_EQ(Actual.OpaqueInputStateGroups, FrozenExpected.OpaqueInputStateGroups);
			EXPECT_EQ(Actual.PipelineTransitions, FrozenExpected.PipelineTransitions);
			EXPECT_EQ(Actual.MaterialUniformGroups.size(), FrozenExpected.MaterialUniformGroups.size());
			for (uint32 Index = 0; Index < Actual.GetNumSections(); ++Index)
			{
				EXPECT_EQ(Actual.GetDraw(Index).SortKey, FrozenExpected.GetDraw(Index).SortKey);
				EXPECT_EQ(Actual.GetDraw(Index).PrimitiveIndex, FrozenExpected.GetDraw(Index).PrimitiveIndex);
				EXPECT_EQ(Actual.GetDraw(Index).ResolvedIndex, FrozenExpected.GetDraw(Index).ResolvedIndex);
				EXPECT_EQ(Actual.GetDraw(Index).Command, FrozenExpected.GetDraw(Index).Command);
				EXPECT_EQ(Actual.GetDraw(Index).MaterialUniformIndex, FrozenExpected.GetDraw(Index).MaterialUniformIndex);
			}
		}
	}
	FrozenInput.reset();
	FrozenExpected = {};

	// A local transform reference and an independently compiled factory must agree.
	FSceneTestOwner ReferenceOwner;
	FSceneTestOwner DeformedOwner;
	for (size_t I = 0; I < 2; ++I)
	{
		auto& PairScene = I == 0 ? *ReferenceOwner : *DeformedOwner;
		auto Proxy = std::make_unique<FProceduralProxy>();
		Proxy->Geometry = Geometries[I]; Proxy->Materials = {Materials[1], Materials[0]};
		FMatrix Transform(1.0);
		if (I == 0)
		{
			Transform[0][0] = Transform[1][1] = Transform[2][2] = 0.8;
			Transform[3][0] = 0.05;
		}
		FSceneInterfaceTestAccess::AddPrimitiveProxy(PairScene, FPrimitiveComponentId(200), std::move(Proxy), Transform);
		FDirectionalLightSceneData Light;
		Light.Intensity = 3.0f;
		ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(PairScene, FLightComponentId(1), Light), nullptr);
	}
	FlushRenderingCommands();
	FSceneView View;
	View.ViewportWidth = 1920; View.ViewportHeight = 1080;
	View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
	View.Settings.DirectionalShadow.Candidate = EDirectionalShadowCandidate::SingleMap;
	for (auto Mode : {ERenderMode::Unlit, ERenderMode::Lit})
	{
		View.Settings.Mode.RenderMode = Mode;
		std::array<FByteBuffer, 2> Pixels;
		std::array<std::array<FByteBuffer, 5>, 2> Attachments;
		static std::array<FByteBuffer, 5>* Capture = nullptr;
		std::array<FByteBuffer, 2> ShadowPixels;
		static FByteBuffer* ShadowCapture = nullptr;
		for (size_t I = 0; I < 2; ++I)
		{
			const auto Desc = FRHITextureCreateDesc::Create2D("GeometryParity", 1920, 1080, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
			const auto Result = RenderPreparedGeometryView(Renderer, I == 0 ? *ReferenceOwner : *DeformedOwner,
				View, Desc, [&] {
				Capture = &Attachments[I];
				ShadowCapture = &ShadowPixels[I];
				if (Mode == ERenderMode::Lit) SetShadowDepthCaptureSink(+[](FRHICommandListImmediate& Commands, FRHITexture* Depth, uint32 Cascades) {
					EXPECT_EQ(Cascades, 1u); ReadGeometryTexture(Commands, Depth, *ShadowCapture, true);
				});
				if (Mode == ERenderMode::Lit) SetGBufferCaptureSink(+[](FRHICommandListImmediate& Commands,
					FRHITexture* Material, FRHITexture* Normals, FRHITexture* Surface, FRHITexture* Emissive, FRHITexture* Depth) {
					const std::array Textures{Material, Normals, Surface, Emissive, Depth};
					for (size_t A = 0; A < Textures.size(); ++A) ReadGeometryTexture(Commands, Textures[A], (*Capture)[A], A == 4);
				});
			}, [&](FRHICommandListImmediate& Commands, FRHITexture* Target) {
				ReadGeometryTexture(Commands, Target, Pixels[I]);
			});
			ShadowCapture = nullptr;
			Capture = nullptr;
			ASSERT_EQ(Result, ERenderViewResult::Success);
		}
		ASSERT_EQ(Pixels[0].size(), Pixels[1].size());
		ASSERT_EQ(Pixels[0].size(), 1920u * 1080u * 4u);
		size_t OutsideTolerance = 0;
		uint32 MaxError = 0;
		for (size_t Pixel = 0; Pixel < Pixels[0].size(); Pixel += 4)
		{
			uint32 Error = 0;
			for (size_t Channel = 0; Channel < 4; ++Channel)
				Error = std::max(Error, static_cast<uint32>(std::abs(std::to_integer<int>(Pixels[0][Pixel + Channel]) - std::to_integer<int>(Pixels[1][Pixel + Channel]))));
			MaxError = std::max(MaxError, Error); OutsideTolerance += Error > 2;
		}
		EXPECT_LE(MaxError, 8u);
		EXPECT_LE(OutsideTolerance, 1920u * 1080u / 1000u);
		if (Mode == ERenderMode::Lit)
		{
			for (size_t A = 0; A < 5; ++A)
			{
				ASSERT_EQ(Attachments[0][A].size(), 1920u * 1080u * 4u) << A;
				ASSERT_EQ(Attachments[0][A].size(), Attachments[1][A].size()) << A;
			}
			size_t Covered = 0;
			for (size_t Offset = 0; Offset < Attachments[0][4].size(); Offset += sizeof(float))
			{
				float Depth[2];
				for (size_t I = 0; I < 2; ++I) std::memcpy(&Depth[I], Attachments[I][4].data() + Offset, sizeof(float));
				ASSERT_TRUE(std::isfinite(Depth[0]) && std::isfinite(Depth[1]));
				ASSERT_EQ(Depth[0] < 1.0f, Depth[1] < 1.0f) << Offset;
				if (Depth[0] >= 1.0f) continue;
				++Covered;
				ASSERT_NEAR(Depth[0], Depth[1], 1e-5f) << Offset;
				// These constant material values have identical quantized encodings;
				// exact comparison is stricter than the frozen float-channel allowance.
				for (size_t A = 0; A < 4; ++A)
					for (size_t Byte = 0; Byte < 4; ++Byte)
						ASSERT_EQ(Attachments[0][A][Offset + Byte], Attachments[1][A][Offset + Byte]) << A << ":" << Offset;
			}
			ASSERT_FALSE(ShadowPixels[0].empty());
			ASSERT_EQ(ShadowPixels[0].size(), ShadowPixels[1].size());
			size_t ShadowCovered = 0;
			for (size_t Offset = 0; Offset < ShadowPixels[0].size(); Offset += sizeof(float))
			{
				float Depth[2];
				for (size_t I = 0; I < 2; ++I) std::memcpy(&Depth[I], ShadowPixels[I].data() + Offset, sizeof(float));
				ASSERT_EQ(Depth[0] < 1.0f, Depth[1] < 1.0f) << Offset;
				ASSERT_NEAR(Depth[0], Depth[1], 1e-5f) << Offset;
				ShadowCovered += Depth[0] < 1.0f;
			}
			EXPECT_GT(ShadowCovered, 100u);
			EXPECT_GT(Covered, 1000u);
			EXPECT_LT(Covered, 1920u * 1080u / 2u);
		}
	}

	ReferenceOwner.Reset(); DeformedOwner.Reset();
	// Optional GBuffer exclusion must preserve retained-forward and translucency.
	FSceneTestOwner ForwardOwner;
	FProceduralGeometry ForwardGeometry;
	const auto Translucent = MakeMaterial(EMaterialBlendMode::Translucent, true);
	FMaterialRenderData TranslucentData;
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
		ForwardGeometry = FProceduralGeometry::Create(Commands, 500, true);
		ForwardGeometry.Binding->bForwardOnly = true;
		TranslucentData = Translucent->GetMaterialWithFallback();
	});
	FlushRenderingCommands();
	FDirectionalLightSceneData ForwardLight;
	ForwardLight.Intensity = 3.0f;
	ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(*ForwardOwner, FLightComponentId(2), ForwardLight), nullptr);
	for (const auto [LocalFactory, FailShadow] : std::array{std::pair{true, false}, std::pair{false, false}, std::pair{false, true}, std::pair{false, false}})
	{
		auto Proxy = std::make_unique<FProceduralProxy>();
		Proxy->Geometry = LocalFactory ? Geometries[0] : ForwardGeometry;
		Proxy->Materials = {Materials[0], TranslucentData};
		Proxy->bFailShadow = FailShadow;
		ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*ForwardOwner, FPrimitiveComponentId(300), std::move(Proxy), FMatrix(1.0)));
		FlushRenderingCommands();
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			FSceneView View;
			View.ViewportWidth = 192; View.ViewportHeight = 108;
			View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.Mode.RenderMode = ERenderMode::Lit;
			View.Settings.DirectionalShadow.Candidate = EDirectionalShadowCandidate::SingleMap;
			const auto Prepared = PrepareStaticMeshView_RenderThread(Commands, ForwardOwner->GetPrimitiveSceneInfos(), View, ERasterMode::Solid);
			ASSERT_EQ(Prepared.Opaque.size(), 1u);
			ASSERT_EQ(Prepared.Translucent.size(), 1u);
			EXPECT_EQ(Prepared.Opaque[0].Command->bSupportsGBuffer, LocalFactory);
			const auto Shadow = PrepareStaticMeshView_RenderThread(Commands, ForwardOwner->GetPrimitiveSceneInfos(), View, ERasterMode::Solid, ERenderPreparationMode::ShadowDepth);
			EXPECT_EQ(Shadow.bResourceFailure, FailShadow);
		});
		FlushRenderingCommands();
		FSceneView View;
		View.ViewportWidth = 192; View.ViewportHeight = 108;
		View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		View.Settings.Mode.RenderMode = ERenderMode::Lit;
		View.Settings.DirectionalShadow.Candidate = EDirectionalShadowCandidate::SingleMap;
		static FViewRenderTelemetry Telemetry;
		Telemetry = {};
		const auto Result = RenderPreparedGeometryView(Renderer, *ForwardOwner, View,
			FRHITextureCreateDesc::Create2D("ForwardOnlyGeometry", 192, 108, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource),
			[] { SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) { Telemetry = Value; }); });
		ASSERT_EQ(Result, FailShadow ? ERenderViewResult::RendererResourcesUnavailable : ERenderViewResult::Success);
		if (FailShadow) continue;
		EXPECT_EQ(Telemetry.GBuffer.GBufferSuccessfulDraws, LocalFactory ? 1u : 0u);
		EXPECT_EQ(Telemetry.CombinedTranslucentGeometryDraws, 1u);
		EXPECT_EQ(Telemetry.DirectionalShadow.ShadowPreparationFailures, FailShadow ? 1u : 0u);
		EXPECT_EQ(Telemetry.DirectionalShadow.ShadowSuccessfulDraws, FailShadow ? 0u : 1u);
	}
	ForwardOwner.Reset();
	FlushRenderingCommands();
	ForwardGeometry = {};
	// Replacement and detach cannot invalidate retained frame bindings/resources.
	auto OldBuffer = Snapshot.Opaque.front().Command->Vertices.Buffer;
	for (uint64 Cycle = 0; Cycle < 100; ++Cycle)
	{
		const std::weak_ptr<const FVertexFactoryInputBinding> Retired = Geometries[0].Binding;
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& CommandList) {
			Geometries[0] = FProceduralGeometry::Create(CommandList, Cycle + 3, false);
		});
		FlushRenderingCommands();
		auto Proxy = std::make_unique<FProceduralProxy>();
		Proxy->Geometry = Geometries[0]; Proxy->Materials = {Materials[1], Materials[0]};
		ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(100), std::move(Proxy), FMatrix(1.0)));
		FlushRenderingCommands();
		EXPECT_EQ(Scene.GetPrimitiveSceneInfos().size(), 2u);
		if (Cycle > 0) EXPECT_TRUE(Retired.expired()) << Cycle;
	}
	EXPECT_EQ(Snapshot.Opaque.front().Command->Vertices.Buffer, OldBuffer);
	EXPECT_NE(Geometries[0].Vertices.Buffer, OldBuffer);
	Owner.Reset();
	FlushRenderingCommands();
	OldBuffer = {};
	Snapshot = {};
	Geometries = {};
}

TEST(FMaterialAnimationVulkanTests, CachedCommandsInvalidateResourceChangesAndReuseNumericUpdates)
{
	using namespace Durin;
	using namespace Durin::Tests;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("EarlyMeshCommands");
	Lifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(Lifecycle);
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionTextureSampleParameter2D> Sample(
			NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, NAME_None));
		Sample->Id = FGuid::NewGuid();
		Sample->Metadata = {.Id = FGuid::NewGuid(), .Name = "EarlyCommandTexture"};
		TStrongObjectPtr<DMaterialExpressionScalarParameter> Brightness(
			NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
		Brightness->Id = FGuid::NewGuid();
		Brightness->Metadata = {.Id = FGuid::NewGuid(), .Name = "EarlyCommandBrightness"};
		Brightness->DefaultValue = 1.0f;
		TStrongObjectPtr<DMaterialExpressionMultiply> Product(NewObject<DMaterialExpressionMultiply>(nullptr, NAME_None));
		Product->Id = FGuid::NewGuid();
		Product->ResultType = EMaterialProgramValueType::Float3;
		Product->A = {Sample->Id, 1};
		Product->B = {Brightness->Id};
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.BaseColor = {Product->Id};
		const std::array<DMaterialExpression*, 3> Expressions{Sample.Get(), Brightness.Get(), Product.Get()};
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		ASSERT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::Ready);
		FSceneTestOwner Owner;
		FProceduralGeometry Geometry;
		std::array<FMeshGeometryRecord::FRef, 2> Publications;
		FMaterialRenderData Data;
		FTextureReference Reference;
		std::array<FTextureRHIRef, 2> Textures;
		const auto MaterialProxy = Material->GetRenderProxy();
		const FGuid TextureParameterId = Sample->Metadata.Id;
		const FGuid BrightnessParameterId = Brightness->Metadata.Id;
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			const std::array<std::array<uint8, 4>, 2> Colors{{{255, 0, 0, 255}, {0, 255, 0, 255}}};
			for (size_t Index = 0; Index < Textures.size(); ++Index)
			{
				Textures[Index] = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("EarlyCommandTexture", 1, 1, EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource));
				ASSERT_TRUE(Textures[Index]);
				Commands.UpdateTexture2D(Textures[Index], 0, 0,
					{0, 0, 0, 0, 1, 1}, 4, std::as_bytes(std::span(Colors[Index])));
				Commands.TransitionTextures(std::array{Durin::FRHITextureTransition{
					Textures[Index], {Durin::ERHITextureAspect::Color, 0, 1, 0, 1},
					Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}});
			}
			Reference.SetReferencedTexture_RenderThread(Textures[0]);
			Data = MaterialProxy->GetMaterialWithFallback();
			FMaterialRenderRepresentationBuilder Builder(Data.Representation);
			ASSERT_TRUE(Builder.SetTexture(TextureParameterId, Reference.GetTextureReferenceRHI()));
			FMaterialRenderValidationDiagnostic Diagnostic;
			ASSERT_TRUE(Builder.Build(Data.Representation, Diagnostic));
			FProceduralProxy Proxy;
			Proxy.Geometry = FProceduralGeometry::Create(Commands, 1, false);
			Proxy.Materials = {Data, Data};
			ASSERT_TRUE(Proxy.PublishGeometry());
			Geometry = Proxy.Geometry;
			Publications = Proxy.Publications;
		});
		FlushRenderingCommands();
		FMatrix Transform(1.0);
		auto Publish = [&](bool bVisible = true) {
			auto Proxy = std::make_unique<FProceduralProxy>();
			Proxy->Geometry = Geometry;
			Proxy->Publications = Publications;
			Proxy->Materials = {Data, Data};
			EXPECT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Owner,
				FPrimitiveComponentId(410), std::move(Proxy), Transform, bVisible));
			FlushRenderingCommands();
		};
		Publish();
		FSceneView View;
		View.ViewportWidth = 192; View.ViewportHeight = 108;
		View.Settings.Mode.RenderMode = ERenderMode::Unlit;
		View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		View.MaterialTimeSeconds = 0;
		// Initial command creation belongs to asynchronous loading. The observed
		// sequence starts warm, then mutates only bindings/values, never shader programs.
		ASSERT_EQ(RenderPreparedGeometryView(Renderer, *Owner, View,
			FRHITextureCreateDesc::Create2D("EarlyCommandPreparation", 192, 108, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)),
			ERenderViewResult::Success);
		std::array<FByteBuffer, 12> Pixels;
		static FViewRenderTelemetry Telemetry;
		for (size_t Frame = 0; Frame < Pixels.size(); ++Frame)
		{
			if (Frame == 5)
			{
				FMaterialRenderRepresentationBuilder Builder(Data.Representation);
				ASSERT_TRUE(Builder.SetScalar(BrightnessParameterId, 0.25f));
				FMaterialRenderValidationDiagnostic Diagnostic;
				ASSERT_TRUE(Builder.Build(Data.Representation, Diagnostic));
				Publish();
			}
			if (Frame == 6) { Transform = Math::ScaleMatrix(FVector3(0.5)); Owner->UpdatePrimitiveTransform(FPrimitiveComponentId(410), Transform); }
			if (Frame == 8) Owner->UpdatePrimitiveVisibility(FPrimitiveComponentId(410), false);
			if (Frame == 9) Owner->UpdatePrimitiveVisibility(FPrimitiveComponentId(410), true);
			if (Frame == 10)
			{
				FMaterialRenderRepresentationBuilder Builder(Data.Representation);
				FMaterialSamplerState Sampler;
				Sampler.MagFilter = EMaterialSamplerMagFilter::Nearest;
				ASSERT_TRUE(Builder.SetTexture(TextureParameterId, Reference.GetTextureReferenceRHI(), Sampler));
				FMaterialRenderValidationDiagnostic Diagnostic;
				ASSERT_TRUE(Builder.Build(Data.Representation, Diagnostic));
				Publish();
			}
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
				if (Frame == 2) Reference.SetReferencedTexture_RenderThread(Textures[1]);
				if (Frame == 4) Reference.Clear_RenderThread();
				View.MaterialTimeSeconds = Frame;
				const auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("EarlyCommandOutput", 192, 108, EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy));
				SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) { Telemetry = Value; });
				FGeometryQualificationFrame FrameScope(Commands);
				const auto Result = Renderer.RenderView(Commands, {&*Owner, View, Target, false, {}});
				SetViewRenderTelemetrySink(nullptr);
				ASSERT_EQ(Result, ERenderViewResult::Success);
				ReadGeometryTexture(Commands, Target, Pixels[Frame]);
			});
			FlushRenderingCommands();
			ASSERT_EQ(Pixels[Frame].size(), 192u * 108u * 4u) << Frame;
			if (Frame == 8)
			{
				EXPECT_EQ(Telemetry.StaticMesh.StaticMeshSuccessfulDraws, 0u);
				EXPECT_NE(Pixels[Frame], Pixels[Frame - 1]);
			}
			else if (Frame == 5)
			{
				// Replacing the primitive replaces the resource owner, even with the same component ID.
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandHits, 0u);
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandMisses, 2u);
				EXPECT_NE(Pixels[Frame], Pixels[Frame - 1]);
			}
			else if (Frame == 2 || Frame == 4)
			{
				// Stable texture references resolve their new target/fallback while recording.
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandHits, 2u);
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandMisses, 0u);
				EXPECT_EQ(Telemetry.StaticMesh.SurfaceMaterialPreparations, 0u);
				EXPECT_NE(Pixels[Frame], Pixels[Frame - 1]);
			}
			else if (Frame == 6)
			{
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandHits, 2u);
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandMisses, 0u);
				EXPECT_NE(Pixels[Frame], Pixels[Frame - 1]);
			}
			else if (Frame == 0 || Frame == 1 || Frame == 3 || Frame == 7 || Frame == 9 || Frame == 11)
			{
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandHits, 2u) << Frame;
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandMisses, 0u) << Frame;
				EXPECT_EQ(Telemetry.StaticMesh.PrimitiveUniformUploads, 0u) << Frame;
				EXPECT_EQ(Telemetry.StaticMesh.SurfaceMaterialPreparations, 0u) << Frame;
				if (Frame != 0) EXPECT_EQ(Pixels[Frame], Pixels[Frame == 9 ? 7 : Frame - 1]) << Frame;
			}
			else
			{
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandHits, 0u) << Frame;
				EXPECT_EQ(Telemetry.StaticMesh.PersistentCommandMisses, 2u) << Frame;
				EXPECT_GT(Telemetry.StaticMesh.SurfaceMaterialPreparations, 0u) << Frame;
				if (Frame == 10) EXPECT_EQ(Pixels[Frame], Pixels[Frame - 1]);
				else if (Frame != 0) EXPECT_NE(Pixels[Frame], Pixels[Frame - 1]) << Frame;
			}
			if (Frame == 6) EXPECT_EQ(Telemetry.StaticMesh.PrimitiveUniformUploads, 0u);
		}
		Owner.Reset();
		FlushRenderingCommands();
	}

}

TEST(FMaterialAnimationVulkanTests, MaterialTimeChangesPixelsWithoutReplacingCachedCommands)
{
	using namespace Durin;
	using namespace Durin::Tests;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness RendererLifecycle("MaterialTimeQualification");
	RendererLifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(RendererLifecycle);
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionTime> Time(NewObject<DMaterialExpressionTime>(nullptr, NAME_None));
		Time->Id = FGuid::NewGuid();
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.BaseColor = {Time->Id};
		const std::array<DMaterialExpression*, 1> Expressions{Time.Get()};
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		ASSERT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::Ready);
		FSceneTestOwner Owner;
		auto Proxy = std::make_unique<FProceduralProxy>();
		const auto MaterialProxy = Material->GetRenderProxy();
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			Proxy->Geometry = FProceduralGeometry::Create(Commands, 1, false);
			const auto Resolved = MaterialProxy->GetMaterialWithFallback();
			Proxy->Materials = {Resolved, Resolved};
			ASSERT_TRUE(Proxy->PublishGeometry());
		});
		FlushRenderingCommands();
		FSceneInterfaceTestAccess::AddPrimitiveProxy(*Owner,
			FPrimitiveComponentId(400), std::move(Proxy), FMatrix(1.0));
		FlushRenderingCommands();
		FSceneView PreparedView;
		PreparedView.ViewportWidth = 192; PreparedView.ViewportHeight = 108;
		PreparedView.Settings.Mode.RenderMode = ERenderMode::Unlit;
		PreparedView.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		PreparedView.MaterialTimeSeconds = 0.125;
		ASSERT_EQ(RenderPreparedGeometryView(Renderer, *Owner, PreparedView,
			FRHITextureCreateDesc::Create2D("MaterialTimePreparation", 192, 108, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)),
			ERenderViewResult::Success);
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			FSceneView View;
			View.ViewportWidth = 192; View.ViewportHeight = 108;
			View.Settings.Mode.RenderMode = ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
			const auto Target = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("MaterialTime", 192, 108, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy));
			std::array<FByteBuffer, 3> Pixels;
			FPreparedStaticMeshView First;
			FMeshDrawDescriptionCache Templates;
			const std::array Times{0.125, 0.75, 0.125};
			for (size_t Frame = 0; Frame < Times.size(); ++Frame)
			{
				View.MaterialTimeSeconds = Times[Frame];
				FGeometryQualificationFrame FrameScope(Commands);
				Templates.BeginSubmission();
				FStaticMeshPreparationCache Cache;
				Cache.Commands = &Templates;
				const auto Prepared = PrepareStaticMeshView_RenderThread(Commands,
					Owner->GetPrimitiveSceneInfos(), View, ERasterMode::Solid, ERenderPreparationMode::Full, &Cache);
				Templates.EndSubmission();
				ASSERT_EQ(Prepared.Opaque.size(), 2u);
				if (Frame == 0) First = Prepared;
				else for (size_t Draw = 0; Draw < First.Opaque.size(); ++Draw)
					EXPECT_EQ(First.Opaque[Draw].Command, Prepared.Opaque[Draw].Command);
				ASSERT_EQ(Renderer.RenderView(Commands, {&*Owner, View, Target, false, {}}), ERenderViewResult::Success);
				ReadGeometryTexture(Commands, Target, Pixels[Frame]);
			}
			ASSERT_EQ(Pixels[0].size(), 192u * 108u * 4u);
			ASSERT_EQ(Pixels[0].size(), Pixels[1].size());
			EXPECT_EQ(Pixels[0], Pixels[2]);
			size_t BrighterPixels = 0;
			for (size_t Pixel = 0; Pixel < Pixels[0].size(); Pixel += 4)
				BrighterPixels += std::to_integer<int>(Pixels[1][Pixel])
					> std::to_integer<int>(Pixels[0][Pixel]) + 32;
			EXPECT_GT(BrighterPixels, 1000u);
		});
		FlushRenderingCommands();
		Owner.Reset();
	}

}

TEST(FMaterialCustomDataVulkanTests, StableInstancesMatchDynamicMaterialsAcrossPassesAndGPUGrouping)
{
	using namespace Durin;
	using namespace Durin::Tests;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("PrimitiveInstanceData");
	Lifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(Lifecycle);
	{
		TStrongObjectPtr<DMaterial> Custom(NewObject<DMaterial>(nullptr, NAME_None)), Reference(NewObject<DMaterial>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionInstanceCustomData> Color(NewObject<DMaterialExpressionInstanceCustomData>(nullptr, NAME_None)),
			Mask(NewObject<DMaterialExpressionInstanceCustomData>(nullptr, NAME_None)), Offset(NewObject<DMaterialExpressionInstanceCustomData>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionPrimitiveCustomData> PrimitiveOffset(NewObject<DMaterialExpressionPrimitiveCustomData>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionAdd> Sum(NewObject<DMaterialExpressionAdd>(nullptr, NAME_None));
		Color->Id = FGuid::NewGuid(); Color->ResultType = EMaterialProgramValueType::Float3; Color->DataIndex = 3;
		Mask->Id = FGuid::NewGuid(); Mask->DataIndex = 31;
		Offset->Id = FGuid::NewGuid(); Offset->ResultType = EMaterialProgramValueType::Float3; Offset->DataIndex = 6;
		PrimitiveOffset->Id = FGuid::NewGuid(); PrimitiveOffset->ResultType = EMaterialProgramValueType::Float3; PrimitiveOffset->DataIndex = 29;
		Sum->Id = FGuid::NewGuid(); Sum->ResultType = EMaterialProgramValueType::Float3;
		Sum->A = {Offset->Id}; Sum->B = {PrimitiveOffset->Id};
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.BaseColor = {Color->Id}; Outputs.OpacityMask = {Mask->Id}; Outputs.WorldPositionOffset = {Sum->Id};
		// Evaluate object-space data in the fragment stage too. A grouped draw must
		// fetch the selected instance instead of its representative primitive.
		TStrongObjectPtr<DMaterialExpressionObjectPosition> ObjectPosition(NewObject<DMaterialExpressionObjectPosition>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionCameraPosition> CameraPosition(NewObject<DMaterialExpressionCameraPosition>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionDistance> ObjectDistance(NewObject<DMaterialExpressionDistance>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionMultiply> PixelColor(NewObject<DMaterialExpressionMultiply>(nullptr, NAME_None));
		ObjectPosition->Id = FGuid::NewGuid(); CameraPosition->Id = FGuid::NewGuid();
		ObjectDistance->Id = FGuid::NewGuid(); ObjectDistance->A = {ObjectPosition->Id}; ObjectDistance->B = {CameraPosition->Id};
		PixelColor->Id = FGuid::NewGuid(); PixelColor->ResultType = EMaterialProgramValueType::Float3;
		PixelColor->A = {Color->Id}; PixelColor->B = {ObjectDistance->Id};
		Outputs.Emissive = {PixelColor->Id};
		const std::array<DMaterialExpression*, 9> Expressions{Color.Get(), Mask.Get(), Offset.Get(), PrimitiveOffset.Get(), Sum.Get(), ObjectPosition.Get(), CameraPosition.Get(), ObjectDistance.Get(), PixelColor.Get()};
		FMaterialStaticProperties Properties; Properties.BlendMode = EMaterialBlendMode::Masked;
		Properties.GeometryUsage = 5; Properties.PassUsage = 7;
		Properties.bTwoSided = true; Properties.OpacityMaskThreshold = .5f;
		ASSERT_TRUE(Custom->SetStaticProperties(Properties));
		ASSERT_TRUE(Custom->SetMaterialExpressions(Expressions, Outputs));
		TStrongObjectPtr<DMaterialExpressionVector4Parameter> Tint(NewObject<DMaterialExpressionVector4Parameter>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionSwizzle> ColorChannels(NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionVector4Parameter> DynamicOffset(NewObject<DMaterialExpressionVector4Parameter>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionSwizzle> OffsetChannels(NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None));
		Tint->Id = FGuid::NewGuid(); Tint->Metadata = {.Id = FGuid::NewGuid(), .Name = "InstanceTint"};
		ColorChannels->Id = FGuid::NewGuid(); ColorChannels->Input = {Tint->Id}; ColorChannels->Components = {0, 1, 2};
		DynamicOffset->Id = FGuid::NewGuid(); DynamicOffset->Metadata = {.Id = FGuid::NewGuid(), .Name = "InstanceOffset"};
		OffsetChannels->Id = FGuid::NewGuid(); OffsetChannels->Input = {DynamicOffset->Id}; OffsetChannels->Components = {0, 1, 2};
		FMaterialExpressionSurfaceOutputs ReferenceOutputs;
		ReferenceOutputs.BaseColor = {ColorChannels->Id}; ReferenceOutputs.OpacityMask.SetConstant({1.f});
		ReferenceOutputs.WorldPositionOffset = {OffsetChannels->Id};
		TStrongObjectPtr<DMaterialExpressionMultiply> ReferencePixelColor(NewObject<DMaterialExpressionMultiply>(nullptr, NAME_None));
		ReferencePixelColor->Id = FGuid::NewGuid(); ReferencePixelColor->ResultType = EMaterialProgramValueType::Float3;
		ReferencePixelColor->A = {ColorChannels->Id}; ReferencePixelColor->B = {ObjectDistance->Id};
		ReferenceOutputs.Emissive = {ReferencePixelColor->Id};
		const std::array<DMaterialExpression*, 8> ReferenceExpressions{Tint.Get(), ColorChannels.Get(), DynamicOffset.Get(), OffsetChannels.Get(), ObjectPosition.Get(), CameraPosition.Get(), ObjectDistance.Get(), ReferencePixelColor.Get()};
		ASSERT_TRUE(Reference->SetStaticProperties(Properties));
		ASSERT_TRUE(Reference->SetMaterialExpressions(ReferenceExpressions, ReferenceOutputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Custom);
		FAssetCompilingManager::Get().FinishCompilationForObject(*Reference);
		if (Custom->GetMaterialCompileStatus().State != EMaterialCompileState::Ready)
			for (const auto& Diagnostic : Custom->GetMaterialCompileDiagnostics()) ADD_FAILURE() << Diagnostic.Message;
		ASSERT_EQ(Custom->GetMaterialCompileStatus().State, EMaterialCompileState::Ready);
		ASSERT_EQ(Reference->GetMaterialCompileStatus().State, EMaterialCompileState::Ready);
		const auto Program = Custom->GetAcceptedCompiledProgram();
		auto Geometry = MakeRenderData();
		Geometry->LODResources[0].Sections.resize(1);
		Geometry->RecalculateBounds();
		EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([&](FRHICommandListImmediate& Commands) {
			ASSERT_TRUE(Geometry->InitResources(Commands));
		});
		FlushRenderingCommands();
		std::vector<FStaticMeshRenderInstance> Instances;
		std::vector<TStrongObjectPtr<DMaterialInstance>> Dynamics;
		FSceneTestOwner Instanced, Dynamic;
		FPrimitiveCustomData PrimitiveData;
		ASSERT_TRUE(PrimitiveData.Set(29, std::array{0.f, 0.f, .02f}));
		for (uint32 Index = 0; Index < 64; ++Index)
		{
			FStaticMeshRenderInstance Instance{.Id = Index + 1};
			Instance.LocalToPrimitive[0][0] = Instance.LocalToPrimitive[1][1] = Instance.LocalToPrimitive[2][2] = .09;
			Instance.LocalToPrimitive[3] = FVector4(-.85 + .24 * (Index % 8), -.85 + .24 * (Index / 8), .2, 1.0);
			const float Strength = .4f + .009f * Index;
			const std::array<float, 3> Value = Index % 2 ? std::array{0.f, Strength, 0.f} : std::array{Strength, 0.f, 0.f};
			ASSERT_TRUE(Instance.CustomData.Set(3, Value));
			const std::array OffsetValues{Index % 2 ? .005f : -.005f, 0.f, .02f + .0002f * Index};
			ASSERT_TRUE(Instance.CustomData.Set(6, OffsetValues));
			ASSERT_TRUE(Instance.CustomData.Set(31, std::array{1.f}));
			Instances.push_back(Instance);
			Dynamics.emplace_back(DMaterialInstance::CreateDynamic(Reference.Get(), nullptr, NAME_None));
			ASSERT_TRUE(Dynamics.back()->SetVectorParameterValue("InstanceTint", FVector4(Value[0], Value[1], Value[2], 1.0)));
			const float CombinedZ = .02f + OffsetValues[2];
			ASSERT_TRUE(Dynamics.back()->SetVectorParameterValue("InstanceOffset", FVector4(OffsetValues[0], 0.0, CombinedZ, 0.0)));
			FSceneInterfaceTestAccess::AddPrimitiveProxy(*Dynamic, FPrimitiveComponentId(2000 + Index),
				std::make_unique<FStaticMeshSceneProxy>(Geometry.get(), std::vector{Dynamics.back()->GetRenderProxy()}), Instance.LocalToPrimitive);
		}
		auto Proxy = std::make_unique<FStaticMeshSceneProxy>(Geometry.get(), std::vector{Custom->GetRenderProxy()},
			std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
		ASSERT_TRUE(Proxy->SetCustomPrimitiveData(PrimitiveData));
		FSceneInterfaceTestAccess::AddPrimitiveProxy(*Instanced, FPrimitiveComponentId(1000), std::move(Proxy), FMatrix(1.0));
		for (auto* Scene : {Instanced.Get(), Dynamic.Get()})
		{
			FDirectionalLightSceneData Light; Light.Intensity = 3.f;
			ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(*Scene, FLightComponentId(1000), Light), nullptr);
		}
		FlushRenderingCommands();
		struct FObservation
		{
			size_t MaterialGroups = 0;
			FByteBuffer Pixels, Shadow;
			std::array<FByteBuffer, 5> GBuffer;
			FViewRenderTelemetry Telemetry;
			FGPUTimingQueryRHIRef RasterTime, CullTime, ShadowTime;
            uint64 PreparationNanoseconds = 0;
			uint64 RecordingNanoseconds = 0;
		};
		auto VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		bool bDynamicInstancing = true;
        bool bAsyncCompute = false;
        FVector3 ViewOrigin(0.0);
		auto CullingFailure = EGPUCullingFailure::None;
		bool bTimePasses = false;
		bool bCaptureOutputs = true;
        auto ShadowCandidate = EDirectionalShadowCandidate::SingleMap;
		auto Render = [&](FScene* Scene, bool GPU, ERenderMode Mode, FObservation& Result) {
			static FObservation* Sink = nullptr;
			FSceneView View; View.ViewportWidth = 192; View.ViewportHeight = 192;
			View.ViewLocation = ViewOrigin; View.ViewMatrix = Math::TranslationMatrix(-ViewOrigin);
            if (ShadowCandidate == EDirectionalShadowCandidate::ThreeCascades)
            {
                // Cascades require positive +X camera depths. Map the fixture's XY
                // planes onto that camera basis, with near/far distances 1 and 11.
                View.ViewLocation = FVector3(0.0, 0.0, -2.0);
                View.ViewMatrix = FMatrix(0.0);
                View.ViewMatrix[2][0] = 1.0;
                View.ViewMatrix[0][1] = 1.0;
                View.ViewMatrix[1][2] = 1.0;
                View.ViewMatrix[3][0] = 2.0;
                View.ViewMatrix[3][3] = 1.0;
                View.ProjectionMatrix = FMatrix(0.0);
                View.ProjectionMatrix[1][0] = 1.0;
                View.ProjectionMatrix[2][1] = -1.0;
                View.ProjectionMatrix[0][2] = 0.1;
                View.ProjectionMatrix[3][2] = -0.1;
                View.ProjectionMatrix[3][3] = 1.0;
            }
            View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.Settings.Mode.RenderMode = Mode;
			View.Settings.Mode.VisibilityMode = VisibilityMode;
			View.Settings.DirectionalShadow.Candidate = ShadowCandidate;
			FSceneViewRenderOptions Options; Options.bEnableGPUCulling = GPU; Options.bEnableDynamicInstancing = bDynamicInstancing; Options.bEnableAsyncCompute = bAsyncCompute;
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
				Result.MaterialGroups = PrepareStaticMeshView_RenderThread(Commands, Scene->GetPrimitiveSceneInfos(),
					View, ERasterMode::Solid).MaterialUniformGroups.size();
			});
			FlushRenderingCommands();
			const auto Outcome = RenderPreparedGeometryView(Renderer, *Scene, View,
				FRHITextureCreateDesc::Create2D("CustomNumericData", 192, 192, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy),
				[&] {
					Sink = &Result;
					if (bTimePasses)
					{
						SetGBufferTimingQuerySink(+[](const FGPUTimingQueryRHIRef& Q) { Sink->RasterTime = Q; });
						SetGPUCullingTimingQuerySink(+[](const FGPUTimingQueryRHIRef& Q) { Sink->CullTime = Q; });
                        SetShadowDepthTimingQuerySink(+[](const FGPUTimingQueryRHIRef& Q) { Sink->ShadowTime = Q; });
					}
					SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) { Sink->Telemetry = Value; });
					if (bCaptureOutputs) SetGBufferCaptureSink(+[](FRHICommandListImmediate& C, FRHITexture* A, FRHITexture* B, FRHITexture* D, FRHITexture* E, FRHITexture* Z) {
						const std::array Textures{A, B, D, E, Z};
						for (size_t I = 0; I < Textures.size(); ++I) ReadGeometryTexture(C, Textures[I], Sink->GBuffer[I], I == 4);
					});
					if (bCaptureOutputs) SetShadowDepthCaptureSink(+[](FRHICommandListImmediate& C, FRHITexture* Texture, uint32) { ReadGeometryTexture(C, Texture, Sink->Shadow, true); });
				},
				[&](FRHICommandListImmediate& Commands, FRHITexture* Target) { if (bCaptureOutputs) ReadGeometryTexture(Commands, Target, Result.Pixels); }, Options, CullingFailure, &Result.PreparationNanoseconds, &Result.RecordingNanoseconds);
			SetGBufferTimingQuerySink(nullptr);
			SetGPUCullingTimingQuerySink(nullptr);
            SetShadowDepthTimingQuerySink(nullptr);
			Sink = nullptr;
			EXPECT_EQ(Outcome, ERenderViewResult::Success);
		};
        auto ResolveCounts = [&](const FObservation& Observation) -> std::optional<FGPUCullingVisibilityCounts> {
            const auto Query = Observation.Telemetry.GBuffer.GPUCullingVisibility;
            if (!Query) return {};
            for (uint32 Poll = 0; Poll < 128 && !Query->GetCounts(); ++Poll)
            {
                EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([](FRHICommandListImmediate& Commands) {
                    FGeometryQualificationFrame Frame(Commands); Commands.PollTextureReadbacks();
                    Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
                });
                FlushRenderingCommands();
            }
            return Query->GetCounts();
        };
		FObservation Direct, Grouped, Baseline;
		for (const auto Mode : {ERenderMode::Unlit, ERenderMode::Lit})
		{
			bDynamicInstancing = false;
			Render(Instanced.Get(), false, Mode, Direct);
			Render(Dynamic.Get(), true, Mode, Baseline);
			bDynamicInstancing = true;
			FObservation CPUMerged; Render(Instanced.Get(), false, Mode, CPUMerged);
			EXPECT_EQ(CPUMerged.Pixels, Direct.Pixels); EXPECT_EQ(CPUMerged.GBuffer, Direct.GBuffer); EXPECT_EQ(CPUMerged.Shadow, Direct.Shadow);
			EXPECT_EQ(Direct.Telemetry.StaticMesh.NativeDrawSubmissions, 64u);
			EXPECT_EQ(CPUMerged.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);
			if (Mode == ERenderMode::Lit)
			{
				EXPECT_EQ(Direct.Telemetry.DirectionalShadow.ShadowNativeDrawSubmissions, 64u);
				EXPECT_EQ(CPUMerged.Telemetry.DirectionalShadow.ShadowNativeDrawSubmissions, 1u);
			}
			Render(Instanced.Get(), true, Mode, Grouped);
			EXPECT_EQ(Direct.Pixels, Baseline.Pixels);
			EXPECT_EQ(Direct.Pixels, Grouped.Pixels);
			ASSERT_FALSE(Direct.Pixels.empty());
			std::array<size_t, 2> Dominant{};
			for (size_t Pixel = 0; Pixel + 3 < Direct.Pixels.size(); Pixel += 4)
			{
				const int Red = std::to_integer<int>(Direct.Pixels[Pixel]);
				const int Green = std::to_integer<int>(Direct.Pixels[Pixel + 1]);
				Dominant[0] += Red > 30 && Red > Green * 2;
				Dominant[1] += Green > 30 && Green > Red * 2;
			}
			EXPECT_GT(Dominant[0], 100u); EXPECT_GT(Dominant[1], 100u);
			if (Mode == ERenderMode::Lit)
			{
				EXPECT_EQ(Direct.GBuffer, Grouped.GBuffer); EXPECT_EQ(Direct.GBuffer, Baseline.GBuffer);
				ASSERT_FALSE(Direct.GBuffer[0].empty());
				EXPECT_EQ(Direct.Shadow, Grouped.Shadow); EXPECT_EQ(Direct.Shadow, Baseline.Shadow);
				ASSERT_FALSE(Direct.Shadow.empty());
				EXPECT_EQ(Grouped.Telemetry.GBuffer.GPUCullingGroups, 1u);
				EXPECT_EQ(Grouped.Telemetry.GBuffer.GPUCullingIndirectDraws, 1u);
				EXPECT_EQ(Grouped.Telemetry.GBuffer.GPUCullingCandidates, 64u);
				EXPECT_EQ(Baseline.Telemetry.GBuffer.GPUCullingIndirectDraws, 0u);
				EXPECT_EQ(Baseline.Telemetry.GBuffer.GBufferSuccessfulDraws, 64u);
				EXPECT_EQ(Direct.Telemetry.GBuffer.GBufferSuccessfulDraws, 64u);
				EXPECT_EQ(Grouped.MaterialGroups, 1u); EXPECT_EQ(Baseline.MaterialGroups, 64u);
				std::cout << "[CustomData] dynamic_draws=" << Baseline.Telemetry.GBuffer.GBufferSuccessfulDraws
					<< " direct_draws=" << Direct.Telemetry.GBuffer.GBufferSuccessfulDraws
					<< " grouped_indirect_draws=" << Grouped.Telemetry.GBuffer.GPUCullingIndirectDraws
					<< " fallback_uniform_abi_bytes=" << sizeof(RendererPrivate::FMeshFallbackPrimitiveUniform) << '\n';
			}
		}
		// Small groups use CPU IDs with either GPU visibility policy. Growing the
		// scene allocation must late-rebind cached commands to the new attributes.
		for (const uint32 Count : {3u, 65u})
		{
			auto Layout = Instances;
			if (Count < Layout.size()) Layout.resize(Count);
			else { auto Extra = Layout.back(); Extra.Id = 1001; Extra.LocalToPrimitive[3].x = 0.0; Layout.push_back(Extra); }
			Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Layout));
			FObservation D, CPU, Policy;
			bDynamicInstancing = false; Render(Instanced.Get(), false, ERenderMode::Lit, D);
			bDynamicInstancing = true; Render(Instanced.Get(), false, ERenderMode::Lit, CPU);
			EXPECT_EQ(D.Pixels, CPU.Pixels); EXPECT_EQ(D.GBuffer, CPU.GBuffer); EXPECT_EQ(D.Shadow, CPU.Shadow);
			EXPECT_EQ(D.Telemetry.StaticMesh.NativeDrawSubmissions, Count);
			EXPECT_EQ(CPU.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);
			if (Count == 3)
			{
				Render(Instanced.Get(), true, ERenderMode::Lit, Policy);
				EXPECT_EQ(Policy.Pixels, D.Pixels); EXPECT_EQ(Policy.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);
				EXPECT_EQ(Policy.Telemetry.GBuffer.GPUCullingDirectFallbackDraws, 1u);
			}
		}
		Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
		// Independently created source descriptions with equal effective state
		// share GPU admission instead of partitioning by wrapper identity.
		{
			class FIndependentBindingProxy final : public FStaticMeshSceneProxy
			{
			public:
				using FStaticMeshSceneProxy::FStaticMeshSceneProxy;
                bool bDynamicSceneData = false;
				auto CollectMeshBatches(const FMeshCollectionContext& Context, FMeshBatchCollector& Collector) const -> void override
				{
					FMeshBatchCollector Collected(Context.Purpose);
					FStaticMeshSceneProxy::CollectMeshBatches(Context, Collected);
					for (auto Batch : Collected.GetBatches())
					{
						if (!Geometry)
						{
							const auto Record = Batch.GeometryRecord->WithBinding(std::make_unique<FStaticMeshBatchBinding>(
								static_cast<const FStaticMeshBatchBinding&>(*Batch.GeometryRecord->GetBinding())));
							ASSERT_TRUE(Record); Geometry = *Record;
						}
						Batch.GeometryRecord = Geometry; Batch.Binding = Geometry->GetBinding();
                        if (bDynamicSceneData) Batch.PrimitiveIdMode = EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData;
						EXPECT_EQ(Collector.Add(std::move(Batch)), EGeometrySubmissionOutcome::Submitted);
					}
				}
			private:
				mutable FMeshGeometryRecord::FRef Geometry;
			};
			auto Small = std::vector<FStaticMeshRenderInstance>(Instances.begin(), Instances.begin() + 3);
			for (size_t Index = 0; Index < Small.size(); ++Index)
			{
				auto& Transform = Small[Index].LocalToPrimitive;
				Transform[0][0] = Transform[1][1] = Transform[2][2] = .03;
				Transform[3] = FVector4(-.7 + .4 * Index, .96, .2, 1.0);
			}
			auto Extra = std::make_unique<FIndependentBindingProxy>(Geometry.get(), std::vector{Custom->GetRenderProxy()},
				std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Small));
			ASSERT_TRUE(Extra->SetCustomPrimitiveData(PrimitiveData));
			FSceneInterfaceTestAccess::AddPrimitiveProxy(*Instanced, FPrimitiveComponentId(1001), std::move(Extra), FMatrix(1.0));
			FObservation D, CPU, Mixed;
			bDynamicInstancing = false; Render(Instanced.Get(), false, ERenderMode::Lit, D);
			bDynamicInstancing = true; Render(Instanced.Get(), false, ERenderMode::Lit, CPU);
			Render(Instanced.Get(), true, ERenderMode::Lit, Mixed);
			EXPECT_EQ(CPU.Pixels, D.Pixels); EXPECT_EQ(CPU.GBuffer, D.GBuffer); EXPECT_EQ(CPU.Shadow, D.Shadow);
			EXPECT_EQ(Mixed.Pixels, D.Pixels); EXPECT_EQ(Mixed.GBuffer, D.GBuffer); EXPECT_EQ(Mixed.Shadow, D.Shadow);
			EXPECT_EQ(D.Telemetry.StaticMesh.NativeDrawSubmissions, 67u);
			EXPECT_EQ(CPU.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);
			EXPECT_EQ(Mixed.Telemetry.GBuffer.GPUCullingIndirectDraws, 1u);
			EXPECT_EQ(Mixed.Telemetry.GBuffer.GPUCullingCandidates, 67u);
			EXPECT_EQ(Mixed.Telemetry.GBuffer.GPUCullingBelowThresholdCandidates, 0u);
			EXPECT_EQ(Mixed.Telemetry.GBuffer.GPUCullingDirectFallbackDraws, 0u);
			EXPECT_EQ(Mixed.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);

			for (const auto Failure : {EGPUCullingFailure::ShaderUnavailable, EGPUCullingFailure::PipelineUnavailable, EGPUCullingFailure::MetadataBudget, EGPUCullingFailure::CommandAdmission})
			{
				CullingFailure = Failure;
				FObservation Fallback; Render(Instanced.Get(), true, ERenderMode::Lit, Fallback);
				EXPECT_EQ(Fallback.Pixels, D.Pixels); EXPECT_EQ(Fallback.GBuffer, D.GBuffer); EXPECT_EQ(Fallback.Shadow, D.Shadow);
				EXPECT_EQ(Fallback.Telemetry.GBuffer.GPUCullingIndirectDraws, 0u);
				EXPECT_EQ(Fallback.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);
				if (Failure == EGPUCullingFailure::MetadataBudget)
					EXPECT_EQ(Fallback.Telemetry.GBuffer.GPUCullingOverflowCandidates, 67u);
				else EXPECT_EQ(Fallback.Telemetry.GBuffer.GPUCullingDispatchFailureViews, 1u);
			}
			CullingFailure = EGPUCullingFailure::None;
			FObservation Retried; Render(Instanced.Get(), true, ERenderMode::Lit, Retried);
			EXPECT_EQ(Retried.Pixels, Mixed.Pixels); EXPECT_EQ(Retried.GBuffer, Mixed.GBuffer);
			EXPECT_EQ(Retried.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);
			FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Instanced, FPrimitiveComponentId(1001));
			FObservation Removed; Render(Instanced.Get(), true, ERenderMode::Lit, Removed);
			EXPECT_EQ(Removed.Pixels, Grouped.Pixels); EXPECT_EQ(Removed.GBuffer, Grouped.GBuffer);
			EXPECT_EQ(Removed.Telemetry.StaticMesh.NativeDrawSubmissions, 1u);
            auto DynamicInstances = Instances;
            for (auto& Instance : DynamicInstances) { Instance.LocalToPrimitive[3].x += .1; Instance.LocalToPrimitive[3].z = .1; }
            auto DynamicProxy = std::make_unique<FIndependentBindingProxy>(Geometry.get(), std::vector{Custom->GetRenderProxy()},
                std::make_shared<const std::vector<FStaticMeshRenderInstance>>(DynamicInstances));
            DynamicProxy->bDynamicSceneData = true;
            ASSERT_TRUE(DynamicProxy->SetCustomPrimitiveData(PrimitiveData));
            FSceneInterfaceTestAccess::AddPrimitiveProxy(*Instanced, FPrimitiveComponentId(1002), std::move(DynamicProxy), FMatrix(1.0));
            FObservation DualDirect, DualGPU;
            bDynamicInstancing = false; Render(Instanced.Get(), false, ERenderMode::Lit, DualDirect);
            bDynamicInstancing = true; bAsyncCompute = true; Render(Instanced.Get(), true, ERenderMode::Lit, DualGPU);
            EXPECT_EQ(DualGPU.Pixels, DualDirect.Pixels); EXPECT_EQ(DualGPU.GBuffer, DualDirect.GBuffer); EXPECT_EQ(DualGPU.Shadow, DualDirect.Shadow);
            EXPECT_EQ(DualGPU.Telemetry.GBuffer.GPUCullingIndirectDraws, 1u);
            EXPECT_EQ(DualGPU.Telemetry.GBuffer.GPUCullingDispatchedViews, 1u);
            EXPECT_EQ(DualGPU.Telemetry.GBuffer.GPUCullingBufferBytes, 128u * sizeof(FInstanceCullingCandidate) + 128u * sizeof(uint32) + sizeof(FRHIDrawIndexedIndirectArguments));
            EXPECT_GT(DualGPU.Telemetry.GBuffer.GPUCullingSceneCapacityBytes, 0u);
            EXPECT_EQ(DualGPU.Telemetry.GBuffer.GPUCullingStandaloneSceneCapacityBytes, 0u);
            const auto DualCounts = ResolveCounts(DualGPU); ASSERT_TRUE(DualCounts); EXPECT_EQ(DualCounts->Visible, 128u);
            bAsyncCompute = false;
            FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Instanced, FPrimitiveComponentId(1002));

		}
		// Keep a single aggregate primitive intersecting the frustum so the CPU broad
		// phase cannot remove its off-screen instances before indirect submission.
		const bool bProfile = std::getenv("DURIN_PROFILE_MAIN_VIEW_CULLING") != nullptr;
		for (const uint32 Outside : {0u, 32u})
		{
			auto Layout = Instances;
			for (size_t I = 0; I < Outside; ++I) Layout[I].LocalToPrimitive[3].x += 4.0;
			Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000),
				std::make_shared<const std::vector<FStaticMeshRenderInstance>>(std::move(Layout)));
			for (const bool Enabled : {false, true})
			{
				VisibilityMode = Enabled ? EViewVisibilityMode::Normal : EViewVisibilityMode::FrustumCullingDisabled;
				bTimePasses = Enabled && bProfile;
				const int Pairs = bTimePasses ? 12 : 1;
				for (int Pair = 0; Pair < Pairs; ++Pair)
				{
					FObservation D, G;
					for (bool Indirect : {Pair % 2 == 0, Pair % 2 != 0})
						Render(Instanced.Get(), Indirect, ERenderMode::Lit, Indirect ? G : D);
					EXPECT_EQ(D.Pixels, G.Pixels);
					EXPECT_EQ(D.GBuffer, G.GBuffer);
					EXPECT_EQ(D.Telemetry.GBuffer.GBufferSuccessfulDraws, 64u);
					EXPECT_EQ(G.Telemetry.GBuffer.GPUCullingIndirectDraws, 1u);
					EXPECT_EQ(G.Telemetry.GBuffer.GPUCullingDirectFallbackDraws, 0u);
					const auto& Query = G.Telemetry.GBuffer.GPUCullingVisibility;
					ASSERT_TRUE(Query);
					for (int Poll = 0; Poll < 60; ++Poll)
					{
						const auto Ready = [](const FGPUTimingQueryRHIRef& Q) {
							return Q && Q->GetResult().State == ERHIGPUTimingResultState::Ready;
						};
						if (Query->GetCounts() && (!bTimePasses || (Ready(D.RasterTime) && Ready(G.RasterTime) && Ready(G.CullTime)))) break;
						EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([](FRHICommandListImmediate& Commands) {
							FGeometryQualificationFrame Frame(Commands);
							Commands.PollTextureReadbacks();
							Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
						});
						FlushRenderingCommands();
					}
					const auto Counts = Query->GetCounts();
					ASSERT_TRUE(Counts);
					EXPECT_EQ(Counts->Candidates, 64u);
					EXPECT_EQ(Counts->Culled, Enabled ? Outside : 0u);
					EXPECT_EQ(Counts->Visible, 64u - (Enabled ? Outside : 0u));
					std::cout << "MAIN_VIEW_CULLING outside=" << Outside << " enabled=" << Enabled
						<< " pair=" << Pair << " direct_draws=" << D.Telemetry.GBuffer.GBufferSuccessfulDraws
						<< " indirect_draws=" << G.Telemetry.GBuffer.GPUCullingIndirectDraws
						<< " visible=" << Counts->Visible << " culled=" << Counts->Culled;
					if (bTimePasses)
					{
						for (const auto& Q : {D.RasterTime, G.RasterTime, G.CullTime})
						{
							ASSERT_TRUE(Q);
							ASSERT_EQ(Q->GetResult().State, ERHIGPUTimingResultState::Ready);
						}
						std::cout << " direct_raster_ns=" << D.RasterTime->GetResult().DurationNanoseconds
							<< " grouped_raster_ns=" << G.RasterTime->GetResult().DurationNanoseconds
							<< " compact_ns=" << G.CullTime->GetResult().DurationNanoseconds;
					}
					std::cout << '\n';
				}
			}
		}
		bTimePasses = false;
		VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000),
			std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
        {
            class FConservativeBoundsProxy final : public FStaticMeshSceneProxy
            {
            public:
                using FStaticMeshSceneProxy::FStaticMeshSceneProxy;
                auto GetLocalBounds() const -> FBox override { return FBox(FVector3(-10.0), FVector3(10.0)); }
            };
            auto Outside = Instances; for (auto& Instance : Outside) Instance.LocalToPrimitive[3].x += 4.0;
            FSceneTestOwner Zero;
            auto Proxy = std::make_unique<FConservativeBoundsProxy>(Geometry.get(), std::vector{Custom->GetRenderProxy()},
                std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Outside));
            ASSERT_TRUE(Proxy->SetCustomPrimitiveData(PrimitiveData));
            FSceneInterfaceTestAccess::AddPrimitiveProxy(*Zero, FPrimitiveComponentId(5000), std::move(Proxy), FMatrix(1.0));
            VisibilityMode = EViewVisibilityMode::Normal;
            FObservation D, G;
            bDynamicInstancing = false; Render(Zero.Get(), false, ERenderMode::Lit, D);
            bDynamicInstancing = true; Render(Zero.Get(), true, ERenderMode::Lit, G);
            EXPECT_EQ(G.Pixels, D.Pixels); EXPECT_EQ(G.GBuffer, D.GBuffer);
            EXPECT_EQ(G.Telemetry.GBuffer.GPUCullingCandidates, 64u);
            const auto Counts = ResolveCounts(G); ASSERT_TRUE(Counts);
            EXPECT_EQ(Counts->Visible, 0u); EXPECT_EQ(Counts->Culled, 64u);
            Zero.Reset(); FlushRenderingCommands();
        }
        {
            // The conservative world-relative tolerance is 1e-9: move rejected
            // instances farther than that tolerance at the 1e12 world origin.
            auto Layout = Instances;
            for (size_t Index = 0; Index < 32; ++Index) Layout[Index].LocalToPrimitive[3].x += 4096.0;
            Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Layout));
            ViewOrigin = FVector3(1099511627776.0, -1099511627776.0, 1099511627776.0);
            Instanced->UpdatePrimitiveTransform(FPrimitiveComponentId(1000), Math::TranslationMatrix(ViewOrigin));
            VisibilityMode = EViewVisibilityMode::Normal;
            for (const bool Async : {false, true})
            {
                bAsyncCompute = Async;
                FObservation D, G;
                bDynamicInstancing = false; Render(Instanced.Get(), false, ERenderMode::Lit, D);
                bDynamicInstancing = true; Render(Instanced.Get(), true, ERenderMode::Lit, G);
                EXPECT_EQ(G.Pixels, D.Pixels); EXPECT_EQ(G.GBuffer, D.GBuffer);
                const auto Counts = ResolveCounts(G); ASSERT_TRUE(Counts);
                EXPECT_EQ(Counts->Visible, 32u); EXPECT_EQ(Counts->Culled, 32u);
                EXPECT_EQ(G.Telemetry.GBuffer.GPUCullingAsyncComputeEligibleViews, Async ? 1u : 0u);
            }
            bAsyncCompute = false; ViewOrigin = FVector3(0.0); VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
            Instanced->UpdatePrimitiveTransform(FPrimitiveComponentId(1000), FMatrix(1.0));
            Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
        }
        if (const char* SampleFile = std::getenv("DURIN_GPUSCENE_SAMPLE_FILE"))
        {
            const char* Profile = std::getenv("DURIN_GPUSCENE_SAMPLE_PROFILE");
            const bool bReleaseSampling = Profile && std::string_view(Profile) == "release";
            if (bReleaseSampling)
            {
#if !defined(NDEBUG) || DURIN_WITH_TRACY
                FAIL() << "Release GPUScene sampling requires Release with Tracy disabled.";
#endif
                const char* Validation = std::getenv("DURIN_VULKAN_VALIDATION");
                ASSERT_TRUE(Validation && std::string_view(Validation) == "off");
                // Exclusive external GPU ownership is admitted by the operator;
                // neither this fixture nor a CTest resource lock can establish it.
            }
            const uint32 Warmups = bReleaseSampling ? 30 : 2;
            const uint32 Samples = bReleaseSampling ? 120 : 6;
			// Output parity is checked outside sampling. Readback would otherwise
			// turn the RenderView CPU span into a capture/synchronization measurement.
            bCaptureOutputs = false;
            std::string CSV = "scenario,sample,view,mode,update_ns,prepare_ns,gbuffer_ns,cull_ns,shadow_ns,attribute_upload_bytes,attribute_upload_ranges,buffer_rebuilds,scene_gpu_capacity_bytes,scene_mirror_capacity_bytes,native_draws,shadow_native_draws,persistent_hits,persistent_misses,culling_transient_bytes,shadow_cascades,shadow_target_backend_bytes,record_ns,instance_id_buffer_creates,instance_id_upload_bytes\n";
            auto Statistics = [&] {
                FGPUSceneStatistics Result;
                EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) { Result = Instanced->GPUScene->GetStatistics(); });
                FlushRenderingCommands(); return Result;
            };
            auto WaitTimings = [&](const FObservation& Observation) {
                for (uint32 Poll = 0; Poll < 128; ++Poll)
                {
                    const auto Ready = [](const FGPUTimingQueryRHIRef& Query) {
                        return !Query || Query->GetResult().State == ERHIGPUTimingResultState::Ready;
                    };
                    if (Ready(Observation.RasterTime) && Ready(Observation.CullTime) && Ready(Observation.ShadowTime)) break;
                    EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([](FRHICommandListImmediate& Commands) {
                        FGeometryQualificationFrame Frame(Commands); Commands.PollTextureReadbacks(); Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
                    });
                    FlushRenderingCommands();
                }
            };
            struct FWorkload { const char* Name; uint32 Count; bool Dynamic; uint32 Views; bool Cascades; };
            for (const auto Work : {FWorkload{"static-small", 3, false, 1, false}, FWorkload{"static-large", 256, false, 1, false},
                FWorkload{"dynamic-large", 256, true, 1, false}, FWorkload{"multi-view", 64, false, 2, false},
                FWorkload{"shadow-heavy", 64, false, 1, true}})
            {
                std::vector<FStaticMeshRenderInstance> Layout;
                for (uint32 Index = 0; Index < Work.Count; ++Index)
                {
                    auto Instance = Instances[Index % Instances.size()]; Instance.Id = Index + 1;
                    const uint32 Side = Work.Count > 64 ? 16 : 8;
                    const double Scale = Work.Count > 64 ? .04 : .09;
                    Instance.LocalToPrimitive = Math::ScaleMatrix(FVector3(Scale));
                    Instance.LocalToPrimitive[3] = FVector4(-.85 + 1.7 * (Index % Side) / (Side - 1), -.85 + 1.7 * (Index / Side) / (Side - 1), .2, 1.0);
                    Layout.push_back(Instance);
                }
                Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Layout));
                ShadowCandidate = Work.Cascades ? EDirectionalShadowCandidate::ThreeCascades : EDirectionalShadowCandidate::SingleMap;
                VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled; bTimePasses = true;
                for (uint32 Iteration = 0; Iteration < Warmups + Samples; ++Iteration)
                {
                    uint64 UpdateNanoseconds = 0;
                    if (Work.Dynamic)
                    {
                        const auto UpdateStart = std::chrono::steady_clock::now();
                        Layout[0].LocalToPrimitive[3].z = .2 + .0001 * (Iteration + 1);
                        ASSERT_TRUE(Layout[0].CustomData.Set(3, std::array{.7f + .001f * Iteration, 0.f, 0.f}));
                        Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Layout));
                        FlushRenderingCommands();
                        UpdateNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - UpdateStart).count();
                    }
                    for (uint32 ViewIndex = 0; ViewIndex < Work.Views; ++ViewIndex)
                    {
                        ViewOrigin = FVector3(.02 * ViewIndex, 0.0, 0.0);
                        for (uint32 Order = 0; Order < 3; ++Order)
                        {
                            const uint32 Mode = Iteration % 2 ? 2 - Order : Order;
                            bDynamicInstancing = Mode != 0;
                            const auto Before = Statistics();
                            FObservation Observed; Render(Instanced.Get(), Mode == 2, ERenderMode::Lit, Observed);
                            const auto After = Statistics(); WaitTimings(Observed);
                            const auto Duration = [](const FGPUTimingQueryRHIRef& Query) -> uint64 {
                                if (!Query) return 0;
                                EXPECT_EQ(Query->GetResult().State, ERHIGPUTimingResultState::Ready);
                                return Query->GetResult().DurationNanoseconds;
                            };
                            ASSERT_TRUE(Observed.RasterTime); ASSERT_TRUE(Observed.ShadowTime);
                            EXPECT_EQ(Observed.Telemetry.DirectionalShadow.ShadowCascadeCount, Work.Cascades ? 3u : 1u);
                            EXPECT_EQ(Observed.Telemetry.StaticMesh.PrimitiveUniformUploads, 0u);
                            const auto Upload = After.UploadBytes - Before.UploadBytes;
                            if (Iteration >= Warmups)
                            {
                                if (!Work.Dynamic || Order != 0 || ViewIndex != 0) EXPECT_EQ(Upload, 0u);
                                else { EXPECT_GT(Upload, 0u); EXPECT_LE(Upload, sizeof(FPrimitiveSceneShaderData) + sizeof(FInstanceSceneShaderData) + sizeof(FPrimitiveCustomData)); }
								EXPECT_EQ(After.InstanceIdBufferCreates, Before.InstanceIdBufferCreates);
								EXPECT_EQ(After.InstanceIdUploadBytes, Before.InstanceIdUploadBytes);
                                CSV += std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", Work.Name, Iteration - Warmups, ViewIndex, Mode,
                                    Order == 0 && ViewIndex == 0 ? UpdateNanoseconds : 0, Observed.PreparationNanoseconds, Duration(Observed.RasterTime), Duration(Observed.CullTime), Duration(Observed.ShadowTime),
                                    Upload, After.UploadRanges - Before.UploadRanges, After.BufferRebuilds - Before.BufferRebuilds,
                                    After.CapacityBytes, After.MirrorCapacityBytes, Observed.Telemetry.StaticMesh.NativeDrawSubmissions,
                                    Observed.Telemetry.DirectionalShadow.ShadowNativeDrawSubmissions, Observed.Telemetry.StaticMesh.PersistentCommandHits,
                                    Observed.Telemetry.StaticMesh.PersistentCommandMisses, Observed.Telemetry.GBuffer.GPUCullingBufferBytes, Observed.Telemetry.DirectionalShadow.ShadowCascadeCount, Observed.Telemetry.DirectionalShadow.ShadowTargetBackendBytes, Observed.RecordingNanoseconds,
                                    After.InstanceIdBufferCreates - Before.InstanceIdBufferCreates, After.InstanceIdUploadBytes - Before.InstanceIdUploadBytes);
                            }
                        }
                    }
                }
                std::ofstream Output(std::filesystem::path(SampleFile), std::ios::binary);
                Output.write(CSV.data(), CSV.size()); Output.flush(); ASSERT_TRUE(Output.good());
            }
            std::cout << "GPUScene " << (bReleaseSampling ? "Release" : "diagnostic")
                << " samples: " << SampleFile << " warmups=" << Warmups << " samples=" << Samples
                << " per mode/view; external quiet-lane admission required\n";
            bCaptureOutputs = true; bTimePasses = false; bDynamicInstancing = true; ViewOrigin = FVector3(0.0); ShadowCandidate = EDirectionalShadowCandidate::SingleMap;
            Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
        }
		auto FindFragmentType = [](std::string_view Entry) -> const FShaderType* {
			for (const auto* Type : FShaderType::GetTypeList())
				if (Type->GetFrequency() == EShaderFrequency::Fragment && Type->GetEntryPoint() == Entry) return Type;
			return nullptr;
		};
		// Local geometry uses the same entry for direct and GPU-selected instance IDs.
		EXPECT_EQ(Custom->GetAcceptedCompiledProgram(), Program);
		Properties.GeometryUsage = 1;
		ASSERT_TRUE(Custom->SetStaticProperties(Properties));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Custom);
		ASSERT_EQ(Custom->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders.size(), 4u);
		FObservation LocalOnly; Render(Instanced.Get(), true, ERenderMode::Lit, LocalOnly);
		EXPECT_EQ(LocalOnly.Pixels, Grouped.Pixels);
		EXPECT_EQ(LocalOnly.Telemetry.GBuffer.GPUCullingIndirectDraws, 1u);
		EXPECT_EQ(LocalOnly.Telemetry.GBuffer.GBufferSuccessfulDraws, 64u);
		{
			using namespace RendererPrivate;
			const auto Selected = Custom->GetAcceptedCompiledProgram();
			const FMeshMaterialShaderMapIdentity Identity{.RenderLayout = Selected->GetShaderMap().Layout.Identity,
				.ProgramIdentity = Selected->GetShaderMap().ShaderMapId};
			FMeshMaterialShaderMap Rejected;
			const auto Before = GetShaderBuildStats().Compilations;
			const auto Factory = FindMeshVertexFactory(FXxHash64::HashBuffer("SplineVertexFactory", sizeof("SplineVertexFactory") - 1));
			const auto* Fragment = FindFragmentType("FragmentMain");
			ASSERT_TRUE(Factory); ASSERT_NE(Fragment, nullptr);
			const auto MissingGeometry = InitializeMaterialShaderMap(*Factory->GetShaderType(MaterialMeshPassForward),
				*Fragment, Factory->GetType(), MaterialMeshPassForward,
				Identity, {}, Selected.get(), {}, Rejected);
			EXPECT_FALSE(MissingGeometry); EXPECT_FALSE(Rejected);
			EXPECT_EQ(MissingGeometry.Status, EShaderBuildStatus::Error);
			EXPECT_EQ(GetShaderBuildStats().Compilations, Before);
		}
		Properties.PassUsage = 1;
		ASSERT_TRUE(Custom->SetStaticProperties(Properties));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Custom);
		{
			using namespace RendererPrivate;
			const auto Selected = Custom->GetAcceptedCompiledProgram();
			ASSERT_EQ(Selected->GetShaderMap().CompiledShaders.size(), 2u);
			const FMeshMaterialShaderMapIdentity Identity{.RenderLayout = Selected->GetShaderMap().Layout.Identity,
				.ProgramIdentity = Selected->GetShaderMap().ShaderMapId};
			FMeshMaterialShaderMap Rejected;
			const auto Before = GetShaderBuildStats().Compilations;
			const auto Factory = FindMeshVertexFactory(FXxHash64::HashBuffer("LocalVertexFactory", sizeof("LocalVertexFactory") - 1));
			const auto* Fragment = FindFragmentType("OpaqueShadowFragmentMain");
			ASSERT_TRUE(Factory); ASSERT_NE(Fragment, nullptr);
			const auto MissingPass = InitializeMaterialShaderMap(*Factory->GetShaderType(MaterialMeshPassShadow),
				*Fragment, Factory->GetType(), MaterialMeshPassShadow,
				Identity, {}, Selected.get(), {}, Rejected);
			EXPECT_FALSE(MissingPass); EXPECT_FALSE(Rejected);
			EXPECT_EQ(MissingPass.Status, EShaderBuildStatus::Error);
			EXPECT_EQ(GetShaderBuildStats().Compilations, Before);
		}
		Properties.GeometryUsage = 5; Properties.PassUsage = 7;
		ASSERT_TRUE(Custom->SetStaticProperties(Properties));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Custom);
		const auto RestoredProgram = Custom->GetAcceptedCompiledProgram();
		ASSERT_TRUE(RestoredProgram);
		std::reverse(Instances.begin(), Instances.end());
		Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
		FObservation Reordered; Render(Instanced.Get(), true, ERenderMode::Lit, Reordered);
		EXPECT_EQ(Reordered.Pixels, Grouped.Pixels);
		ASSERT_TRUE(PrimitiveData.Set(29, std::array{.04f, 0.f, .02f}));
		Instanced->UpdatePrimitiveCustomData(FPrimitiveComponentId(1000), PrimitiveData);
		FObservation Moved; Render(Instanced.Get(), true, ERenderMode::Lit, Moved);
		EXPECT_NE(Moved.Pixels, Reordered.Pixels);
		EXPECT_EQ(Moved.Telemetry.GBuffer.GPUCullingIndirectDraws, 1u);
		ASSERT_TRUE(Instances.front().CustomData.Set(31, std::array{0.f}));
		Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
		FObservation Masked; Render(Instanced.Get(), true, ERenderMode::Lit, Masked);
		EXPECT_EQ(Masked.Telemetry.GBuffer.GPUCullingIndirectDraws, 1u);
		EXPECT_NE(Masked.GBuffer[4], Moved.GBuffer[4]);
		EXPECT_NE(Masked.Shadow, Moved.Shadow);
		Instances.pop_back();
		Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>(Instances));
		FObservation Changed, Repeated;
		Render(Instanced.Get(), false, ERenderMode::Lit, Changed);
		Render(Instanced.Get(), false, ERenderMode::Lit, Repeated);
		EXPECT_NE(Changed.Pixels, Grouped.Pixels); EXPECT_EQ(Changed.Pixels, Repeated.Pixels);
		EXPECT_EQ(Changed.Telemetry.GBuffer.GBufferSuccessfulDraws, 63u);
		EXPECT_EQ(Custom->GetAcceptedCompiledProgram(), RestoredProgram);
		Instanced->UpdateStaticMeshRenderInstances(FPrimitiveComponentId(1000), std::make_shared<const std::vector<FStaticMeshRenderInstance>>());
		FObservation Empty; Render(Instanced.Get(), false, ERenderMode::Unlit, Empty);
		EXPECT_EQ(Empty.Telemetry.StaticMesh.PreparedStaticMeshPrimitives, 0u);
		Instanced.Reset(); Dynamic.Reset();
		FlushRenderingCommands();
		EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([&](FRHICommandListImmediate&) { Geometry->ReleaseResources(); });
		FlushRenderingCommands();
	}

}

TEST(FMaterialParameterCollectionVulkanTests,
	WorldSnapshotsDriveForwardGBufferAndMaskedShadowValues)
{
	using namespace Durin;
	using namespace Durin::Tests;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness RendererLifecycle("MaterialCollectionQualification");
	RendererLifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(RendererLifecycle);
	{
		TStrongObjectPtr<DMaterialParameterCollection> Collection(
			NewObject<DMaterialParameterCollection>(nullptr, "GPUCollection"));
		const FGuid ColorId = FGuid::NewGuid();
		const FGuid MaskId = FGuid::NewGuid();
		const std::array Declarations{
			FMaterialParameterCollectionDeclaration{.Id = ColorId,
				.Name = FName("Color"), .Type = EMaterialParameterType::Vector,
				.DefaultValue = FVector4(1.0, 0.0, 0.0, 0.0)},
			FMaterialParameterCollectionDeclaration{.Id = MaskId,
				.Name = FName("Mask"), .Type = EMaterialParameterType::Scalar,
				.DefaultValue = FVector4(1.0, 0.0, 0.0, 0.0)}};
		ASSERT_TRUE(Collection->SetDeclarations(Declarations));
		auto Layout = Collection->BuildLayout();
		ASSERT_TRUE(Layout);

		TStrongObjectPtr<DMaterial> Material(
			NewObject<DMaterial>(nullptr, "GPUCollectionMaterial"));
		TStrongObjectPtr<DMaterialExpressionCollectionParameter> Color(
			NewObject<DMaterialExpressionCollectionParameter>(nullptr, "GPUCollectionColor"));
		TStrongObjectPtr<DMaterialExpressionCollectionParameter> Mask(
			NewObject<DMaterialExpressionCollectionParameter>(nullptr, "GPUCollectionMask"));
		Color->Id = FGuid::NewGuid(); Color->Collection = Collection.Get();
		Color->ParameterId = ColorId;
		Mask->Id = FGuid::NewGuid(); Mask->Collection = Collection.Get();
		Mask->ParameterId = MaskId;
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.BaseColor = {Color->Id};
		Outputs.OpacityMask = {Mask->Id};
		const std::array<DMaterialExpression*, 2> Expressions{Color.Get(), Mask.Get()};
		FMaterialStaticProperties Properties;
		Properties.BlendMode = EMaterialBlendMode::Masked;
		Properties.OpacityMaskThreshold = 0.5f;
		Properties.bTwoSided = true;
		ASSERT_TRUE(Material->SetStaticProperties(Properties));
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		ASSERT_EQ(Material->GetMaterialCompileStatus().State,
			EMaterialCompileState::Ready);

		auto MakeSnapshot = [&](FVector4 ColorValue, float MaskValue, uint64 Version) {
			auto Snapshot = std::make_shared<FMaterialParameterCollectionSnapshot>();
			Snapshot->Layout = *Layout;
			Snapshot->Version = Version;
			Snapshot->Payload = Layout->DefaultPayload;
			auto Write = [&](FGuid Id, const FVector4& Value) {
				const auto Field = std::ranges::find(
					Layout->UniformLayout.Fields, Id,
					&FMaterialRenderField::ParameterId);
				if (Field == Layout->UniformLayout.Fields.end())
				{
					ADD_FAILURE() << "Missing collection field";
					return;
				}
				const std::array<float, 4> Components{
					static_cast<float>(Value.x), static_cast<float>(Value.y),
					static_cast<float>(Value.z), static_cast<float>(Value.w)};
				std::memcpy(Snapshot->Payload.data() + Field->Offset,
					Components.data(), Field->Size);
			};
			Write(ColorId, ColorValue);
			Write(MaskId, FVector4(MaskValue, 0.0, 0.0, 0.0));
			return Snapshot;
		};

		FSceneTestOwner RedWorld;
		FSceneTestOwner GreenWorld;
		RedWorld->UpdateMaterialParameterCollection(
			MakeSnapshot(FVector4(1.0, 0.0, 0.0, 0.0), 1.0f, 1));
		GreenWorld->UpdateMaterialParameterCollection(
			MakeSnapshot(FVector4(0.0, 1.0, 0.0, 0.0), 1.0f, 1));
		const auto MaterialProxy = Material->GetRenderProxy();
		FProceduralGeometry Geometry;
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
			[&](FRHICommandListImmediate& Commands) {
				Geometry = FProceduralGeometry::Create(Commands, 901, false);
				const auto Resolved = MaterialProxy->GetMaterialWithFallback();
				for (FScene* Scene : {RedWorld.Get(), GreenWorld.Get()})
				{
					auto Proxy = std::make_unique<FProceduralProxy>();
					Proxy->Geometry = Geometry;
					Proxy->Materials = {Resolved, Resolved};
					ASSERT_TRUE(Proxy->PublishGeometry());
					FMatrix Transform(1.0);
					Transform[0][0] = Transform[1][1] = Transform[2][2] = 0.8;
					Transform[3][0] = 0.05;
					FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene,
						FPrimitiveComponentId(901), std::move(Proxy), Transform);
				}
			});
		FlushRenderingCommands();
		for (FScene* Scene : {RedWorld.Get(), GreenWorld.Get()})
		{
			FDirectionalLightSceneData Light;
			Light.Intensity = 3.0f;
			ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(
				*Scene, FLightComponentId(901), Light), nullptr);
		}
		FlushRenderingCommands();
		// Exercise command reuse across equal-version
		// world snapshots, changed bindings, incompatible schemas and default fallback.
		std::unordered_set<FScene*> PreparedCollectionScenes;
		auto RenderCollection = [&](FScene* Scene, FByteBuffer& Pixels, bool bExpectedSuccess = true) {
			FViewRenderTelemetry Observation;
			FSceneView View;
			View.ViewportWidth = 192; View.ViewportHeight = 108;
			View.Settings.Mode.RenderMode = ERenderMode::Unlit;
			View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
			if (PreparedCollectionScenes.insert(Scene).second)
			{
				// Prime each scene's fixed program before observing collection-only mutations.
				const auto Outcome = RenderPreparedGeometryView(Renderer, *Scene, View,
					FRHITextureCreateDesc::Create2D("CollectionCachePreparation", 192, 108, EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy));
				EXPECT_EQ(Outcome, ERenderViewResult::Success);
				if (Outcome != ERenderViewResult::Success) return Observation.StaticMesh;
			}
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
				const auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("CollectionCacheCorrectness", 192, 108, EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy));
				ASSERT_TRUE(Target);
				static FViewRenderTelemetry* Sink = nullptr;
				Sink = &Observation;
				SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) { *Sink = Value; });
				FGeometryQualificationFrame FrameScope(Commands);
				const auto Result = Renderer.RenderView(Commands, {Scene, View, Target, false, {}});
				SetViewRenderTelemetrySink(nullptr); Sink = nullptr;
				EXPECT_EQ(Result == ERenderViewResult::Success, bExpectedSuccess);
				if (Result == ERenderViewResult::Success) ReadGeometryTexture(Commands, Target, Pixels);
			});
			FlushRenderingCommands();
			return Observation.StaticMesh;
		};
		FByteBuffer Red, Green, Repeated, Blue;
		RenderCollection(RedWorld.Get(), Red);
		RenderCollection(GreenWorld.Get(), Green);
		EXPECT_NE(Red, Green);
		for (auto* Scene : {RedWorld.Get(), GreenWorld.Get()})
		{
			const auto Stats = RenderCollection(Scene, Repeated);
			EXPECT_EQ(Stats.PersistentCommandHits, 2u);
			EXPECT_EQ(Stats.PersistentCommandMisses, 0u);
			EXPECT_EQ(Stats.SurfaceMaterialPreparations, 0u);
			EXPECT_EQ(Stats.PreparedSurfaceBindingBatches, 0u);
			EXPECT_EQ(Repeated, Scene == RedWorld.Get() ? Red : Green);
		}
		GreenWorld->UpdateMaterialParameterCollection(MakeSnapshot(FVector4(0,0,1,0), 1.0f, 2));
		const auto Updated = RenderCollection(GreenWorld.Get(), Blue);
		EXPECT_EQ(Updated.PersistentCommandHits, 2u);
		EXPECT_EQ(Updated.PersistentCommandMisses, 0u);
		EXPECT_EQ(Updated.SurfaceMaterialPreparations, 0u);
		EXPECT_EQ(Updated.PreparedSurfaceBindingBatches, 0u);
		EXPECT_NE(Blue, Green);
		const auto Stable = RenderCollection(GreenWorld.Get(), Repeated);
		EXPECT_EQ(Stable.PreparedSurfaceBindingBatches, 0u);
		EXPECT_EQ(Repeated, Blue);
		RenderCollection(RedWorld.Get(), Repeated);
		EXPECT_EQ(Repeated, Red);
		auto Incompatible = MakeSnapshot(FVector4(0,1,0,0), 1.0f, 3);
		++Incompatible->Layout.SchemaVersion;
		GreenWorld->UpdateMaterialParameterCollection(Incompatible);
		const auto Rejected = RenderCollection(GreenWorld.Get(), Repeated);
		EXPECT_EQ(Rejected.StaticMeshSuccessfulDraws, 0u);
		GreenWorld->RemoveMaterialParameterCollection(Layout->CollectionId);
		RenderCollection(GreenWorld.Get(), Repeated);
		EXPECT_EQ(Repeated, Red);
		GreenWorld->UpdateMaterialParameterCollection(MakeSnapshot(FVector4(0,1,0,0), 1.0f, 1));
		RenderCollection(GreenWorld.Get(), Repeated);
		EXPECT_EQ(Repeated, Green);

		FSceneView LitView;
		LitView.ViewportWidth = 192; LitView.ViewportHeight = 108;
		LitView.Settings.Mode.RenderMode = ERenderMode::Lit;
		LitView.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		LitView.Settings.DirectionalShadow.Candidate = EDirectionalShadowCandidate::SingleMap;
		for (auto* Scene : {RedWorld.Get(), GreenWorld.Get()})
			ASSERT_EQ(RenderPreparedGeometryView(Renderer, *Scene, LitView,
				FRHITextureCreateDesc::Create2D("CollectionLitPreparation", 192, 108, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)),
				ERenderViewResult::Success);
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
			[&](FRHICommandListImmediate& Commands) {
				FSceneView View;
				View.ViewportWidth = 192; View.ViewportHeight = 108;
				View.Settings.Mode.VisibilityMode =
					EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadow.Candidate =
					EDirectionalShadowCandidate::SingleMap;
				std::array<FByteBuffer, 2> ForwardPixels;
				for (size_t I = 0; I < 2; ++I)
				{
					View.Settings.Mode.RenderMode = ERenderMode::Unlit;
					const auto Target = GDynamicRHI->RHICreateTexture(Commands,
						FRHITextureCreateDesc::Create2D("CollectionForward", 192, 108,
							EPixelFormat::SRGBA8_UNORM).SetFlags(
								ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy));
					FGeometryQualificationFrame FrameScope(Commands);
					ASSERT_EQ(Renderer.RenderView(Commands, {I == 0 ? RedWorld.Get() : GreenWorld.Get(), View, Target, false, {}}), ERenderViewResult::Success);
					ReadGeometryTexture(Commands, Target, ForwardPixels[I]);
				}
				size_t RedDominant = 0, GreenDominant = 0;
				for (size_t Pixel = 0; Pixel < ForwardPixels[0].size(); Pixel += 4)
				{
					RedDominant += std::to_integer<int>(ForwardPixels[0][Pixel])
						> std::to_integer<int>(ForwardPixels[0][Pixel + 1]) + 32;
					GreenDominant += std::to_integer<int>(ForwardPixels[1][Pixel + 1])
						> std::to_integer<int>(ForwardPixels[1][Pixel]) + 32;
				}
				EXPECT_GT(RedDominant, 1000u);
				EXPECT_GT(GreenDominant, 1000u);

				std::array<FByteBuffer, 2> GBufferMaterial;
				std::array<FByteBuffer, 2> ShadowDepth;
				static FByteBuffer* GBufferCapture = nullptr;
				static FByteBuffer* ShadowCapture = nullptr;
				static FViewRenderTelemetry Telemetry;
				View.Settings.Mode.RenderMode = ERenderMode::Lit;
				for (size_t I = 0; I < 2; ++I)
				{
					Telemetry = {};
					SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) {
						Telemetry = Value;
					});
					const auto Target = GDynamicRHI->RHICreateTexture(Commands,
						FRHITextureCreateDesc::Create2D("CollectionDeferred", 192, 108,
							EPixelFormat::SRGBA8_UNORM).SetFlags(
								ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy));
					GBufferCapture = &GBufferMaterial[I];
					ShadowCapture = &ShadowDepth[I];
					SetGBufferCaptureSink(+[](FRHICommandListImmediate& LocalCommands,
						FRHITexture* MaterialTexture, FRHITexture*, FRHITexture*,
						FRHITexture*, FRHITexture*) {
						ReadGeometryTexture(LocalCommands, MaterialTexture,
							*GBufferCapture);
					});
					SetShadowDepthCaptureSink(+[](FRHICommandListImmediate& LocalCommands,
						FRHITexture* Depth, uint32) {
						ReadGeometryTexture(LocalCommands, Depth, *ShadowCapture, true);
					});
					FGeometryQualificationFrame FrameScope(Commands);
					ASSERT_EQ(Renderer.RenderView(Commands, {I == 0 ? RedWorld.Get() : GreenWorld.Get(), View, Target, false, {}}), ERenderViewResult::Success);
					SetGBufferCaptureSink(nullptr);
					SetShadowDepthCaptureSink(nullptr);
					SetViewRenderTelemetrySink(nullptr);
					EXPECT_GT(Telemetry.DirectionalShadow.ShadowSuccessfulDraws, 0u);
				}
				GBufferCapture = nullptr;
				ShadowCapture = nullptr;
				EXPECT_NE(GBufferMaterial[0], GBufferMaterial[1]);
				auto Covered = [](const FByteBuffer& Pixels) {
					size_t Count = 0;
					for (size_t Offset = 0; Offset < Pixels.size(); Offset += sizeof(float))
					{
						float Depth = 1.0f;
						std::memcpy(&Depth, Pixels.data() + Offset, sizeof(float));
						Count += Depth < 1.0f;
					}
					return Count;
				};
				EXPECT_GT(Covered(ShadowDepth[0]), 100u);
				EXPECT_GT(Covered(ShadowDepth[1]), 100u);
			});
		FlushRenderingCommands();

		GreenWorld->UpdateMaterialParameterCollection(
			MakeSnapshot(FVector4(0.0, 1.0, 0.0, 0.0), 0.0f, 2));
		FlushRenderingCommands();
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
			[&](FRHICommandListImmediate& Commands) {
				FSceneView View;
				View.ViewportWidth = 192; View.ViewportHeight = 108;
				View.Settings.Mode.RenderMode = ERenderMode::Lit;
				View.Settings.Mode.VisibilityMode =
					EViewVisibilityMode::FrustumCullingDisabled;
				View.Settings.DirectionalShadow.Candidate =
					EDirectionalShadowCandidate::SingleMap;
				const auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("CollectionMaskedOut", 192, 108,
						EPixelFormat::SRGBA8_UNORM).SetFlags(
							ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::SourceCopy));
				FByteBuffer MaskedShadow;
				static FByteBuffer* ShadowCapture = nullptr;
				ShadowCapture = &MaskedShadow;
				SetShadowDepthCaptureSink(+[](FRHICommandListImmediate& LocalCommands,
					FRHITexture* Depth, uint32) {
					ReadGeometryTexture(LocalCommands, Depth, *ShadowCapture, true);
				});
				FGeometryQualificationFrame FrameScope(Commands);
				ASSERT_EQ(Renderer.RenderView(Commands, {GreenWorld.Get(), View, Target, false, {}}), ERenderViewResult::Success);
				SetShadowDepthCaptureSink(nullptr);
				ShadowCapture = nullptr;
				size_t Covered = 0;
				for (size_t Offset = 0; Offset < MaskedShadow.size(); Offset += sizeof(float))
				{
					float Depth = 1.0f;
					std::memcpy(&Depth, MaskedShadow.data() + Offset, sizeof(float));
					Covered += Depth < 1.0f;
				}
				EXPECT_EQ(Covered, 0u);
			});
		FlushRenderingCommands();
		// Identical local layouts must not merge different Collection contracts.
		TStrongObjectPtr<DMaterialParameterCollection> OtherCollection(NewObject<DMaterialParameterCollection>(nullptr, "OtherGPUCollection"));
		auto OtherDeclarations = Declarations;
		OtherDeclarations[0].DefaultValue = FVector4(0,0,1,0);
		ASSERT_TRUE(OtherCollection->SetDeclarations(OtherDeclarations));
		const auto OtherLayout = OtherCollection->BuildLayout();
		ASSERT_TRUE(OtherLayout);
		TStrongObjectPtr<DMaterial> OtherMaterial(NewObject<DMaterial>(nullptr, "OtherGPUCollectionMaterial"));
		Color->Collection = OtherCollection.Get(); Mask->Collection = OtherCollection.Get();
		ASSERT_TRUE(OtherMaterial->SetStaticProperties(Properties));
		ASSERT_TRUE(OtherMaterial->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*OtherMaterial);
		ASSERT_TRUE(OtherMaterial->GetAcceptedCompiledProgram());
		const auto OtherProxy = OtherMaterial->GetRenderProxy();
		FSceneTestOwner MixedWorld;
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) {
			auto Proxy = std::make_unique<FProceduralProxy>();
			Proxy->Geometry = Geometry;
			Proxy->Materials = {MaterialProxy->GetMaterialWithFallback(), OtherProxy->GetMaterialWithFallback()};
			EXPECT_EQ(Proxy->Materials[0].Representation.GetLayout(), Proxy->Materials[1].Representation.GetLayout());
			ASSERT_TRUE(Proxy->PublishGeometry());
			FSceneInterfaceTestAccess::AddPrimitiveProxy(*MixedWorld, FPrimitiveComponentId(902), std::move(Proxy), FMatrix(1.0));
		});
		FByteBuffer MixedPixels;
		RenderCollection(MixedWorld.Get(), MixedPixels);
		auto CountDominant = [](const FByteBuffer& Pixels, size_t Channel) {
			size_t Count = 0;
			for (size_t Offset = 0; Offset < Pixels.size(); Offset += 4)
				Count += std::to_integer<int>(Pixels[Offset + Channel]) > 100
					&& std::to_integer<int>(Pixels[Offset + (Channel + 1) % 3]) < 50
					&& std::to_integer<int>(Pixels[Offset + (Channel + 2) % 3]) < 50;
			return Count;
		};
		EXPECT_GT(CountDominant(MixedPixels, 0), 100u);
		EXPECT_GT(CountDominant(MixedPixels, 2), 100u);
		auto Broken = MakeSnapshot(FVector4(0,1,0,0), 1.0f, 4);
		++Broken->Layout.SchemaVersion;
		MixedWorld->UpdateMaterialParameterCollection(Broken);
		const auto Partial = RenderCollection(MixedWorld.Get(), Repeated);
		EXPECT_EQ(Partial.StaticMeshSuccessfulDraws, 0u);
		EXPECT_NE(Repeated, MixedPixels);
		FSceneTestOwner PartialWorld;
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) {
			for (uint32 Index = 0; Index < 2; ++Index)
			{
				auto Proxy = std::make_unique<FProceduralProxy>(); Proxy->Geometry = Geometry;
				Proxy->Materials = Index == 0
					? std::array<FMaterialRenderData, 2>{MaterialProxy->GetMaterialWithFallback(), MaterialProxy->GetMaterialWithFallback()}
					: std::array<FMaterialRenderData, 2>{OtherProxy->GetMaterialWithFallback(), OtherProxy->GetMaterialWithFallback()};
				ASSERT_TRUE(Proxy->PublishGeometry());
				FSceneInterfaceTestAccess::AddPrimitiveProxy(*PartialWorld, FPrimitiveComponentId(920 + Index), std::move(Proxy), FMatrix(1.0));
			}
		});
		RenderCollection(PartialWorld.Get(), Repeated);
		PartialWorld->UpdateMaterialParameterCollection(Broken);
		const auto OtherComponent = RenderCollection(PartialWorld.Get(), Repeated);
		EXPECT_EQ(OtherComponent.StaticMeshSuccessfulDraws, 2u);
		MixedWorld->RemoveMaterialParameterCollection(Layout->CollectionId);
		RenderCollection(MixedWorld.Get(), Repeated);
		const auto MixedStable = RenderCollection(MixedWorld.Get(), Repeated);
		EXPECT_EQ(MixedStable.PersistentCommandHits, 2u);
		EXPECT_EQ(MixedStable.PreparedSurfaceBindingBatches, 0u);
		EXPECT_EQ(Repeated, MixedPixels);
		auto OtherSnapshot = MakeSnapshot(FVector4(0,1,0,0), 1.0f, 1);
		OtherSnapshot->Layout = *OtherLayout;
		MixedWorld->UpdateMaterialParameterCollection(OtherSnapshot);
		const auto MixedChanged = RenderCollection(MixedWorld.Get(), Repeated);
		// Collection source changes update material Uniform members, preserving commands.
		EXPECT_EQ(MixedChanged.PersistentCommandHits, 2u);
		EXPECT_EQ(MixedChanged.SurfaceMaterialPreparations, 0u);
		EXPECT_GT(CountDominant(Repeated, 0), 100u);
		EXPECT_GT(CountDominant(Repeated, 1), 100u);
		EXPECT_EQ(CountDominant(Repeated, 2), 0u);
		// Retained programs can carry different default payloads for the same ID.
		// Default values do not participate in shader identity.
		FSceneTestOwner DefaultsWorld;
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) {
			auto Proxy = std::make_unique<FProceduralProxy>();
			Proxy->Geometry = Geometry;
			const auto Primary = MaterialProxy->GetMaterialWithFallback();
			auto Alternate = Primary;
			// Different simultaneous default bindings need an independent material source.
			Alternate.UniformExpressionCache = std::make_shared<FUniformExpressionCache>();
			auto Program = std::make_shared<FMaterialProgramResource>(*Primary.CompiledProgram);
			ASSERT_EQ(Program->ActiveCollections.size(), 1u);
			Program->ActiveCollections[0].DefaultPayload = OtherLayout->DefaultPayload;
			Alternate.CompiledProgram = std::move(Program);
			Proxy->Materials = {Primary, Alternate};
			ASSERT_TRUE(Proxy->PublishGeometry());
			FSceneInterfaceTestAccess::AddPrimitiveProxy(*DefaultsWorld, FPrimitiveComponentId(903), std::move(Proxy), FMatrix(1.0));
		});
		RenderCollection(DefaultsWorld.Get(), MixedPixels);
		EXPECT_GT(CountDominant(MixedPixels, 0), 100u);
		EXPECT_GT(CountDominant(MixedPixels, 2), 100u);
		const auto DefaultsStable = RenderCollection(DefaultsWorld.Get(), Repeated);
		EXPECT_EQ(DefaultsStable.PersistentCommandHits, 2u);
		EXPECT_EQ(DefaultsStable.PreparedSurfaceBindingBatches, 0u);
		EXPECT_EQ(Repeated, MixedPixels);
		DefaultsWorld.Reset();
		MixedWorld.Reset();
		RedWorld.Reset();
		GreenWorld.Reset();
	}

}

TEST(FStaticMeshRenderPreparationVulkanTests, ReplacementRetiresOldResourcesAndInitializesNewResources)
{
	using namespace Durin;
	if (!GIsGameThreadIdInitialized)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
	}
	InitializeDObjectSystem();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, "ReplacementGpuFailure");
	auto Replace = [&] {
		ASSERT_TRUE(Durin::FStaticMeshTestAccess::ReplaceRenderData(Mesh, MakeRenderData(), {
			{.Name = FName("Section0"), .SourceMaterialIndex = 0},
			{.Name = FName("Section1"), .SourceMaterialIndex = 1},
			{.Name = FName("Section2"), .SourceMaterialIndex = 2},
			{.Name = FName("Section3"), .SourceMaterialIndex = 3}}));
	};
	Replace();
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	Mesh->InitResources();
	FlushRenderingCommands();
	ASSERT_TRUE(Mesh->GetRenderResourceStatus().IsReady());
	const auto* ResidentData = Mesh->GetRenderData();
	std::atomic_bool bObservedRelease = false;
	uint64 QueuedRevision = 0;
	{
		FScopedRenderThreadBlocker Blocker;
		Mesh->ReleaseResources();
		Mesh->InitResources();
		Mesh->ReleaseResources();
		// Observe actual resources between release and the following initialization.
		EnqueueRenderCommand("ObserveStaticMeshResourceCycle",
			[ResidentData, &bObservedRelease](FRHICommandListImmediate&) {
				bObservedRelease = ResidentData->GetNumInitializedResources() == 0;
			});
		Mesh->InitResources();
		QueuedRevision = Mesh->GetRenderResourceStatus().Revision;
		EXPECT_TRUE(Mesh->HasPendingRenderResourceInitialization());
		Blocker.Release();
	}
	FlushRenderingCommands();
	EXPECT_TRUE(bObservedRelease);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, QueuedRevision);
	EXPECT_EQ(Mesh->GetRenderData(), ResidentData);
	EXPECT_TRUE(Mesh->GetRenderResourceStatus().IsReady());
	Mesh->ReleaseResources();
	FlushRenderingCommands();
	EXPECT_FALSE(Mesh->GetRenderResourceStatus().IsReady());
	EXPECT_EQ(ResidentData->GetNumInitializedResources(), 0u);
	EXPECT_EQ(Mesh->GetRenderData(), ResidentData);
	Mesh->RequestRenderDataAndResources();
	FlushRenderingCommands();
	EXPECT_TRUE(Mesh->GetRenderResourceStatus().IsReady());
	const auto Revision = Mesh->GetRenderResourceStatus().Revision;
	Replace();
	FlushRenderingCommands();
	ASSERT_NE(Mesh->GetRenderData(), nullptr);
	EXPECT_TRUE(Mesh->GetRenderResourceStatus().IsReady());
	EXPECT_GT(Mesh->GetRenderResourceStatus().Revision, Revision);
	EXPECT_GT(Mesh->GetRenderData()->GetNumInitializedResources(), 0u);
	Mesh->InitResources();
	FlushRenderingCommands();
	EXPECT_TRUE(Mesh->GetRenderResourceStatus().IsReady());
	EXPECT_FALSE(Durin::FStaticMeshTestAccess::ReplaceRenderData(Mesh, nullptr, {}));
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_FALSE(Mesh->RequestRenderDataAndResources());
	EXPECT_FALSE(Mesh->GetRenderResourceStatus().IsReady());
	MarkAsGarbage(Mesh);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	FlushRenderingCommands();
	ShutdownRenderingThread();
	RHIExit();
}

TEST(FStaticMeshRenderPreparationVulkanTests, SplineMirrorsMatchStaticTransformsInPixelsAndNormalMapping)
{
	using namespace Durin;
	using namespace Durin::Tests;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId();
	GIsGameThreadIdInitialized = true;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("SplineMirrorGpuRegression");
	Lifecycle.Start(Renderer);
	{
		TStrongObjectPtr<DMaterial> SingleSided(NewObject<DMaterial>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterial> TwoSided(NewObject<DMaterial>(nullptr, NAME_None));
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.BaseColor.SetConstant({0.8f, 0.2f, 0.1f});
		// A nonzero tangent-space Y makes an incorrect bitangent sign visible
		// in the captured GBuffer, independently of the lighting direction.
		Outputs.Normal.SetConstant({0.3f, 0.6f, 0.74f});
		for (auto* Material : {SingleSided.Get(), TwoSided.Get()})
		{
			FMaterialStaticProperties Properties;
			Properties.bTwoSided = Material == TwoSided.Get();
			ASSERT_TRUE(Material->SetStaticProperties(Properties));
			ASSERT_TRUE(Material->SetMaterialExpressions({}, Outputs));
			FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
			ASSERT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::Ready);
		}
		auto Data = MakeRenderData();
		Data->LODResources[0].Sections.resize(1);
		Data->RecalculateBounds();
		EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([&](FRHICommandListImmediate& Commands) {
			ASSERT_TRUE(Data->InitResources(Commands));
		});
		FlushRenderingCommands();
		FSceneTestOwner Reference, Spline;
		struct FObservation
		{
			FByteBuffer Pixels, Normals, Depth;
		};
		auto Render = [&](FScene* Scene, ERenderMode Mode, FObservation& Observation) {
			FSceneView View;
			View.ViewportWidth = View.ViewportHeight = 96;
			View.Settings.Mode.RenderMode = Mode;
			View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
			static FObservation* Capture = nullptr;
			const auto Result = RenderPreparedGeometryView(Renderer, *Scene, View,
				FRHITextureCreateDesc::Create2D("SplineMirrorRegression", 96, 96, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy),
				[&] {
					Capture = &Observation;
					if (Mode == ERenderMode::Lit)
						SetGBufferCaptureSink(+[](FRHICommandListImmediate& C, FRHITexture*, FRHITexture* Normals,
							FRHITexture*, FRHITexture*, FRHITexture* Depth) {
							ReadGeometryTexture(C, Normals, Capture->Normals);
							ReadGeometryTexture(C, Depth, Capture->Depth, true);
						});
				},
				[&](FRHICommandListImmediate& Commands, FRHITexture* Target) {
					ReadGeometryTexture(Commands, Target, Observation.Pixels);
				});
			Capture = nullptr;
			EXPECT_EQ(Result, ERenderViewResult::Success);
		};
		for (const bool bTwoSided : {false, true})
			for (const double WorldSide : {1.0, -1.0})
				for (const FVector2 Scale : {FVector2(1.0, 1.0), FVector2(-1.0, 1.0),
					FVector2(1.0, -1.0), FVector2(-1.0, -1.0)})
				{
					SCOPED_TRACE(std::format("two-sided={} world-side={} spline-scale=({}, {})", bTwoSided, WorldSide, Scale.x, Scale.y));
					const auto Material = (bTwoSided ? TwoSided : SingleSided)->GetRenderProxy();
					FMatrix World = Math::ScaleMatrix(FVector3(0.75, 0.75 * WorldSide, 0.75));
					World[3][2] = 0.5;
					const FMatrix StaticTransform = World * Math::ScaleMatrix(FVector3(1.0, Scale.x, Scale.y));
					FSplineMeshRenderDynamicData Dynamic;
					Dynamic.Params.StartPosition = {-1.0, 0.0, 0.0};
					Dynamic.Params.EndPosition = {1.0, 0.0, 0.0};
					Dynamic.Params.StartTangent = Dynamic.Params.EndTangent = {2.0, 0.0, 0.0};
					Dynamic.Params.SourceForwardMin = -1.0;
					Dynamic.Params.SourceForwardMax = 1.0;
					Dynamic.Params.StartScale = Dynamic.Params.EndScale = Scale;
					Dynamic.LocalBounds = Data->LocalBounds;
					ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Reference, FPrimitiveComponentId(601),
						std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector{Material}), StaticTransform));
					ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Spline, FPrimitiveComponentId(602),
						std::make_unique<FSplineMeshSceneProxy>(Data.get(), std::vector{Material}, Dynamic), World));
					FlushRenderingCommands();
					// Exercise the same orientation policy used by base and shadow passes.
					EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
						for (const auto Mode : {ERenderPreparationMode::Full, ERenderPreparationMode::ShadowDepth})
						{
							FSceneView View;
							const auto A = PrepareStaticMeshView_RenderThread(Commands, Reference->GetPrimitiveSceneInfos(), View, ERasterMode::Solid, Mode);
							const auto B = PrepareStaticMeshView_RenderThread(Commands, Spline->GetPrimitiveSceneInfos(), View, ERasterMode::Solid, Mode);
							ASSERT_EQ(A.Opaque.size(), 1u);
							ASSERT_EQ(B.Opaque.size(), 1u);
							EXPECT_EQ(A.Opaque[0].Command->PipelineKey.Rasterizer.FrontFace, B.Opaque[0].Command->PipelineKey.Rasterizer.FrontFace);
							EXPECT_EQ(A.Opaque[0].Command->PipelineKey.Rasterizer.CullMode, B.Opaque[0].Command->PipelineKey.Rasterizer.CullMode);
							for (const auto* Prepared : {&A, &B})
							{
								const auto* Primitive = Prepared->GetPrimitive(Prepared->Opaque[0]);
								ASSERT_NE(Primitive, nullptr);
								const auto& Binding = Primitive->CollectedBinding;
								ASSERT_TRUE(Binding && Binding->Declaration);
								const auto Factory = RendererPrivate::FindMeshVertexFactory(Binding->GetFactoryKey());
								ASSERT_TRUE(Factory);
								const FPSOPrecacheVertexFactoryData Precache(&Factory->GetType());
								EXPECT_TRUE(Precache.MatchesVertexElements(Binding->DeclarationElements));
								EXPECT_TRUE(Precache.MatchesVertexElements(Binding->Declaration->GetElements()));
							}
						}
					});
					FlushRenderingCommands();
					for (const auto Mode : {ERenderMode::Unlit, ERenderMode::Lit})
					{
						FObservation A, B;
						Render(Reference.Get(), Mode, A);
						Render(Spline.Get(), Mode, B);
						ASSERT_EQ(A.Pixels.size(), 96u * 96u * 4u);
						EXPECT_EQ(A.Pixels, B.Pixels);
						if (Mode != ERenderMode::Lit) continue;
						ASSERT_EQ(A.Depth.size(), 96u * 96u * sizeof(float));
						EXPECT_EQ(A.Depth, B.Depth);
						ASSERT_EQ(A.Normals.size(), 96u * 96u * 4u);
						EXPECT_EQ(A.Normals, B.Normals);
						size_t Covered = 0;
						for (size_t Offset = 0; Offset < A.Depth.size(); Offset += sizeof(float))
						{
							float Depth;
							std::memcpy(&Depth, A.Depth.data() + Offset, sizeof(float));
							Covered += Depth < 1.0f;
						}
						// Two-sided captures always contain geometry; single-sided
						// mirrors also test the reference's intentionally culled side.
						if (bTwoSided || Scale.y > 0.0) EXPECT_GT(Covered, 1000u);
						else EXPECT_EQ(Covered, 0u);
					}
				}
		// A segment that changes orientation needs both faces in a single draw.
		// Compare single-sided authoring with an explicitly two-sided baseline.
		for (const auto Interpolation : {ESplineMeshInterpolation::Linear, ESplineMeshInterpolation::SmoothStep})
			for (const auto EndScale : {FVector2(2.0, 1.0), FVector2(-1.0, -1.0), FVector2(0.0, 1.0)})
			{
				FSplineMeshRenderDynamicData Dynamic;
				Dynamic.Params.StartPosition = {-1.0, 0.0, 0.0};
				Dynamic.Params.EndPosition = {1.0, 0.0, 0.0};
				Dynamic.Params.StartTangent = Dynamic.Params.EndTangent = {2.0, 0.0, 0.0};
				Dynamic.Params.SourceForwardMin = -1.0;
				Dynamic.Params.SourceForwardMax = 1.0;
				Dynamic.Params.StartScale = {-1.0, 1.0};
				Dynamic.Params.EndScale = EndScale;
				Dynamic.Params.Interpolation = Interpolation;
				Dynamic.LocalBounds = Data->LocalBounds;
				FMatrix World = Math::ScaleMatrix(FVector3(0.4));
				World[3][2] = 0.5;
				ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Reference, FPrimitiveComponentId(601),
					std::make_unique<FSplineMeshSceneProxy>(Data.get(), std::vector{TwoSided->GetRenderProxy()}, Dynamic), World));
				ASSERT_TRUE(FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*Spline, FPrimitiveComponentId(602),
					std::make_unique<FSplineMeshSceneProxy>(Data.get(), std::vector{SingleSided->GetRenderProxy()}, Dynamic), World));
				FlushRenderingCommands();
				EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
					for (const auto Mode : {ERenderPreparationMode::Full, ERenderPreparationMode::ShadowDepth})
					{
						const auto Prepared = PrepareStaticMeshView_RenderThread(Commands, Spline->GetPrimitiveSceneInfos(),
							FSceneView{}, ERasterMode::Solid, Mode);
						ASSERT_EQ(Prepared.Opaque.size(), 1u);
						EXPECT_EQ(Prepared.Opaque[0].Command->PipelineKey.Rasterizer.CullMode, ERHICullMode::None);
					}
				});
				FlushRenderingCommands();
				FObservation A, B;
				Render(Reference.Get(), ERenderMode::Lit, A);
				Render(Spline.Get(), ERenderMode::Lit, B);
				ASSERT_EQ(A.Pixels.size(), 96u * 96u * 4u);
				EXPECT_EQ(A.Pixels, B.Pixels);
				EXPECT_EQ(A.Depth, B.Depth);
				EXPECT_EQ(A.Normals, B.Normals);
				ASSERT_EQ(A.Depth.size(), 96u * 96u * sizeof(float));
				size_t Covered = 0;
				for (size_t Offset = 0; Offset < A.Depth.size(); Offset += sizeof(float))
				{
					float Depth;
					std::memcpy(&Depth, A.Depth.data() + Offset, sizeof(float));
					Covered += Depth < 1.0f;
				}
				EXPECT_GT(Covered, 100u);
			}
		Reference.Reset();
		Spline.Reset();
		EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([&](FRHICommandListImmediate&) { Data->ReleaseResources(); });
		FlushRenderingCommands();
	}
	Lifecycle.Shutdown();
	ShutdownRenderingThread();
	RHIExit();
}

TEST(FStaticMeshRenderPreparationVulkanTests, MaterialPSOPrecacheSharesNativeWorkAndRetiresChangedOwnersAndComponents)
{
	Durin::Tests::FScopedComponentPSOPrecacheMode PrecacheMode(Durin::EComponentPSOPrecacheMode::Enabled);
	using namespace Durin;
	using namespace Durin::Tests;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetRuntime());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("MaterialPSOPrecacheIntegration");
	Lifecycle.Start(Renderer);
	std::vector<FMaterialPSOPrecacheRequestID> SurvivingRequests;
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
		Parameter->Id = FGuid::NewGuid(); Parameter->Metadata = {.Id = FGuid::NewGuid(), .Name = "PSOScalar"};
		Parameter->DefaultValue = 0.5f;
		FMaterialStaticProperties Properties; Properties.ShadingModel = EMaterialShadingModel::Lit;
		ASSERT_TRUE(Material->SetStaticProperties(Properties));
		FMaterialExpressionSurfaceOutputs Outputs; Outputs.Roughness = {Parameter->Id};
		const std::array<DMaterialExpression*, 1> Expressions{Parameter.Get()};
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		ASSERT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::Ready);
		TStrongObjectPtr<DMaterialInstance> Instance(DMaterialInstance::CreateDynamic(Material.Get()));
		ASSERT_NE(Instance.Get(), nullptr);
		const auto Local = RendererPrivate::FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
		const auto Spline = RendererPrivate::FindMeshVertexFactory(FSplineMeshBatchBinding{}.GetFactoryKey());
		ASSERT_TRUE(Local && Spline);
		const std::array Factories{FPSOPrecacheVertexFactoryData(&Local->GetType()), FPSOPrecacheVertexFactoryData(&Spline->GetType())};
		FPSOPrecacheParams Options;
		std::vector<FMaterialPSOPrecacheRequestID> Root, Dynamic, Duplicate;
		ASSERT_TRUE(Material->PrecachePSOs(Renderer, FSceneInterface::GetDefaultFeatureLevel(), Factories, Options, EPSOPrecachePriority::Medium, Root));
		ASSERT_TRUE(Instance->PrecachePSOs(Renderer, FSceneInterface::GetDefaultFeatureLevel(), Factories, Options, EPSOPrecachePriority::Medium, Dynamic));
		ASSERT_TRUE(Material->PrecachePSOs(Renderer, FSceneInterface::GetDefaultFeatureLevel(), Factories, Options, EPSOPrecachePriority::Medium, Duplicate));
		ASSERT_EQ(Root.size(), 2u); ASSERT_EQ(Dynamic.size(), 2u);
		EXPECT_EQ(Root[0].GetValue(), Dynamic[0].GetValue()); EXPECT_EQ(Root[0].GetValue(), Duplicate[0].GetValue());
		Duplicate[0].Cancel();
		ASSERT_TRUE(Instance->SetScalarParameterValue("PSOScalar", 0.2f));
		EXPECT_EQ(Dynamic[0].GetProgress().State, EMaterialPSOPrecacheState::Pending);
		auto PumpUntilFinished = [&](const auto& Requests) {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (std::ranges::any_of(Requests, [](const auto& Id) { return Id.GetProgress().State == EMaterialPSOPrecacheState::Pending; })
				&& std::chrono::steady_clock::now() < Deadline)
			{
				EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) { ++GRenderFrameCounterRenderThread; Renderer.BeginFrame_RenderThread(Commands); });
				FlushRenderingCommands();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			for (const auto& Id : Requests)
			{
				EXPECT_EQ(Id.GetProgress().State, EMaterialPSOPrecacheState::Ready) << Id.GetDiagnostic();
				EXPECT_TRUE(Id.GetProgress().bRequiredReady); EXPECT_EQ(Id.GetProgress().Pipelines, GDynamicRHI->RHIGetCapabilities()->bSupportsNonSolidFill ? 22u : 14u);
			}
		};
		PumpUntilFinished(Dynamic);
		EXPECT_EQ(Root[0].GetProgress().State, EMaterialPSOPrecacheState::Ready);
		// Completion releases the accepted program and native observations without
		// changing the deduplication key for another owner of the same program.
		ASSERT_TRUE(Material->PrecachePSOs(Renderer, FSceneInterface::GetDefaultFeatureLevel(), Factories, Options, EPSOPrecachePriority::Medium, Duplicate));
		EXPECT_EQ(Duplicate[0].GetValue(), Root[0].GetValue());
		EXPECT_EQ(Duplicate[0].GetProgress().State, EMaterialPSOPrecacheState::Ready);
		TStrongObjectPtr<DStaticMeshComponent> Component(NewObject<DStaticMeshComponent>(nullptr, NAME_None));
		uint32 Completions = 0;
		Component->SetMaterialPSOPrecacheData(Dynamic, [&] { ++Completions; });
		EXPECT_EQ(Component->GetPSOPrecacheComponentData().Poll(), EMaterialPSOPrecacheState::Ready);
		EXPECT_EQ(Completions, 1u);
		Component->MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		EXPECT_TRUE(Component->GetPSOPrecacheComponentData().IsPSOPrecacheCalled());
		EXPECT_TRUE(Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().empty());
		EXPECT_EQ(Root[0].GetProgress().State, EMaterialPSOPrecacheState::Ready);
		Properties.bTwoSided = true; ASSERT_TRUE(Material->SetStaticProperties(Properties));
		EXPECT_EQ(Root[0].GetProgress().State, EMaterialPSOPrecacheState::Stale);
		EXPECT_EQ(Dynamic[0].GetProgress().State, EMaterialPSOPrecacheState::Stale);
		ASSERT_TRUE(Material->PrecachePSOs(Renderer, FSceneInterface::GetDefaultFeatureLevel(), Factories, Options, EPSOPrecachePriority::Medium, SurvivingRequests));
		EXPECT_NE(SurvivingRequests[0].GetValue(), Root[0].GetValue());
		PumpUntilFinished(SurvivingRequests);
	}
	Lifecycle.Shutdown();
	for (const auto& Request : SurvivingRequests) EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	ShutdownRenderingThread(); RHIExit();
}

TEST(FStaticMeshRenderPreparationVulkanTests, ComponentsAutomaticallyPrecacheAndGateCurrentProxyBatch)
{
	Durin::Tests::FScopedComponentPSOPrecacheMode PrecacheMode(Durin::EComponentPSOPrecacheMode::Enabled);
	using namespace Durin; using namespace Durin::Tests;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints()); ASSERT_TRUE(InitializeAssetRuntime()); ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext()); ASSERT_NE(GDynamicRHI, nullptr); InitRenderingThread();
	FRendererModule Renderer; FModuleTestHarness Lifecycle("ComponentPSOPrecacheIntegration"); Lifecycle.Start(Renderer);
	struct FRendererCleanup
	{
		FModuleTestHarness& Lifecycle;
		~FRendererCleanup() { Lifecycle.Shutdown(); ShutdownRenderingThread(); RHIExit(); }
	} RendererCleanup{Lifecycle};
	{
		TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
		FMaterialStaticProperties Properties; Properties.ShadingModel = EMaterialShadingModel::Lit;
		ASSERT_TRUE(Material->SetStaticProperties(Properties));
		TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
		Parameter->Id = FGuid::NewGuid(); Parameter->Metadata = {.Id = FGuid::NewGuid(), .Name = "ComponentPSORoughness"}; Parameter->DefaultValue = 0.5f;
		FMaterialExpressionSurfaceOutputs Outputs; Outputs.Roughness = {Parameter->Id};
		const std::array<DMaterialExpression*, 1> Expressions{Parameter.Get()};
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
		auto* Mesh = NewObject<DStaticMesh>(nullptr, NAME_None);
		struct FMeshCleanup
		{
			DStaticMesh* Mesh;
			~FMeshCleanup() {
				auto* Data = FStaticMeshTestAccess::GetMutableRenderData(Mesh);
				if (Data) EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([Data](FRHICommandListImmediate&) { Data->ReleaseResources(); });
				FlushRenderingCommands(); MarkAsGarbage(Mesh); CollectGarbage(Durin::EObjectFlags::Standalone); FlushRenderingCommands();
			}
		} MeshCleanup{Mesh};
		std::vector<FMeshMaterialSlotDefinition> Slots;
		for (uint32 Index = 0; Index < 4; ++Index) Slots.push_back({.Name = FName(std::format("Section{}", Index)), .DefaultMaterial = Material.Get()});
		ASSERT_TRUE(FStaticMeshTestAccess::ReplaceRenderData(Mesh, MakeRenderData(), std::move(Slots)));
		auto SceneOwner = Renderer.CreateScene(); auto& Scene = *static_cast<FScene*>(SceneOwner.get());
		TStrongObjectPtr<DWorld> World(NewObject<DWorld>(nullptr, NAME_None));
		struct FCleanup
		{
			DWorld* World; FScenePtr& Scene;
			~FCleanup() { World->CleanupWorld(); World->SetRenderScene(nullptr); Scene->Release(); Scene.reset(); FlushRenderingCommands(); }
		} Cleanup{World.Get(), SceneOwner};
		ASSERT_NO_THROW(World->InitializeSubsystems());
		auto* Level = NewObject<DLevel>(World.Get(), NAME_None);
		ASSERT_TRUE(World->SetCurrentLevel(Level)); World->SetRenderScene(&Scene);
		auto* Actor = Level->SpawnActor<AActor>(); ASSERT_NE(Actor, nullptr);
		auto* Component = Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(DStaticMeshComponent::StaticClass()));
		ASSERT_NE(Component, nullptr); Component->SetStaticMesh(Mesh);
		ASSERT_TRUE(Component->IsRegistered());
		ASSERT_EQ(Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().size(), 1u) << Component->GetPSOPrecacheDiagnostic();
		FComponentPSOPrecacheParamsList Collected;
		Component->CollectPSOPrecacheData({}, Collected);
		ASSERT_EQ(Collected.size(), 1u);
		EXPECT_TRUE(Collected[0].PSOPrecacheParams.Participation.bCastShadow);
		EXPECT_TRUE(Collected[0].PSOPrecacheParams.Participation.bRenderInMainPass);

		auto OldRequests = std::vector<FMaterialPSOPrecacheRequestID>(Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().begin(),
			Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().end());
		for (const auto& Request : OldRequests) EXPECT_EQ(Request.GetPriority(), EPSOPrecachePriority::Highest);
		auto Count = [&] {
			size_t Result = 0;
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) { Result = Scene.GetPrimitiveSceneInfos().size(); });
			FlushRenderingCommands(); return Result;
		};
		EXPECT_EQ(Count(), 0u); // No explicit precache or first draw has occurred.
		auto Pump = [&](size_t Expected) {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			do {
				EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) { ++GRenderFrameCounterRenderThread; Renderer.BeginFrame_RenderThread(Commands); });
				FlushRenderingCommands(); DPrimitiveComponent::ProcessPSOPrecacheRequests(World.Get());
				if (Count() == Expected) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < Deadline);
			EXPECT_EQ(Count(), Expected) << Component->GetPSOPrecacheDiagnostic();
			for (const auto& Request : Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs())
				EXPECT_TRUE(Request.GetProgress().bRequiredReady) << Request.GetDiagnostic();
		};
		Pump(1);
		// Even an already published proxy must preserve unresolved material demand
		// through normal logical collection, rather than authorizing an error draw.
		const auto MaterialProxy = Material->GetRenderProxy(Scene.GetFeatureLevel());
		const auto Accepted = Material->GetMaterialResource();
		FMaterialResource Compiling;
		Compiling.bIsCompiling = true;
		MaterialProxy->QueuePublication_GameThread(Compiling);
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			EXPECT_EQ(MaterialProxy->GetMaterialNoFallback(), nullptr);
			EXPECT_TRUE(MaterialProxy->IsCompiling());
			EXPECT_TRUE(MaterialProxy->GetMaterialWithFallback().Representation.IsError());
			FRenderPipelinePreparationBatch Batch;
			FSceneView View;
			View.ViewportWidth = View.ViewportHeight = 64;
			(void)PrepareStaticMeshView_RenderThread(Commands, Scene.GetPrimitiveSceneInfos(), View,
				ERasterMode::Solid, ERenderPreparationMode::Full);
			EXPECT_TRUE(FRenderPipelinePreparationBatch::HasPending());
		});
		MaterialProxy->QueuePublication_GameThread(Accepted);
		FlushRenderingCommands();
		const auto Job = Component->GetPSOPrecacheComponentData().GetLatestJobSet();
		EXPECT_EQ(DPrimitiveComponent::GetPendingPSOPrecacheComponentCount(), 0u);
		for (uint32 Frame = 0; Frame < 16; ++Frame)
			DPrimitiveComponent::ProcessPSOPrecacheRequests(World.Get());
		EXPECT_EQ(DPrimitiveComponent::GetPendingPSOPrecacheComponentCount(), 0u);
		EXPECT_EQ(Component->GetPSOPrecacheComponentData().GetLatestJobSet(), Job);
		Component->SetRelativeScale3D(FVector3(-1, 1, 1));
		EXPECT_EQ(Component->GetPSOPrecacheComponentData().GetLatestJobSet(), Job);
		// Renderer invalidation must wake a dormant component even without a
		// material revision or Shader reload-generation change.
		ASSERT_TRUE(Renderer.RequestResourceInvalidation(ERendererResourceInvalidationCause::ManualRetry).bSuccess);
		FlushRenderingCommands();
		EXPECT_EQ(OldRequests[0].GetProgress().State, EMaterialPSOPrecacheState::Stale);
		DPrimitiveComponent::ProcessPSOPrecacheRequests(World.Get());
		EXPECT_GT(Component->GetPSOPrecacheComponentData().GetLatestJobSet(), Job);
		Pump(1);
		EXPECT_EQ(DPrimitiveComponent::GetPendingPSOPrecacheComponentCount(), 0u);
		Properties.bTwoSided = true; ASSERT_TRUE(Material->SetStaticProperties(Properties));
		EXPECT_GT(DPrimitiveComponent::GetPendingPSOPrecacheComponentCount(), 0u);
		EXPECT_EQ(Count(), 0u); // Material invalidation retires the old proxy before another draw.
		DPrimitiveComponent::ProcessPSOPrecacheRequests(World.Get());
		EXPECT_EQ(OldRequests[0].GetProgress().State, EMaterialPSOPrecacheState::Stale);
		Pump(1);
		TStrongObjectPtr<DMaterial> AddedMaterial(NewObject<DMaterial>(nullptr, NAME_None));
		ASSERT_TRUE(Component->SetMaterial(0, AddedMaterial.Get()));
		EXPECT_TRUE(Component->IsPSOPrecaching()); EXPECT_EQ(Count(), 0u);
		Component->SetPSOPrecacheProxyCreationStrategy(EPSOPrecacheProxyCreationStrategy::AlwaysCreate);
		EXPECT_EQ(Count(), 1u); EXPECT_TRUE(Component->IsPSOPrecaching());
		Component->SetPSOPrecacheProxyCreationStrategy(EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached);
		EXPECT_EQ(Count(), 0u);
		SetComponentPSOPrecacheMode(EComponentPSOPrecacheMode::Disabled);
		DPrimitiveComponent::ProcessPSOPrecacheRequests(World.Get());
		EXPECT_EQ(Count(), 1u); // Disabling releases a withheld proxy without waiting for the material.
		EXPECT_FALSE(Component->IsPSOPrecaching());
		EXPECT_TRUE(Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().empty());
		EXPECT_EQ(DPrimitiveComponent::GetPendingPSOPrecacheComponentCount(), 0u);
		EXPECT_EQ(OldRequests[0].GetProgress().State, EMaterialPSOPrecacheState::Stale);
		SetComponentPSOPrecacheMode(EComponentPSOPrecacheMode::Enabled);
		Component->SetPSOPrecacheProxyCreationStrategy(EPSOPrecacheProxyCreationStrategy::AlwaysCreate);
		Component->SetPSOPrecacheProxyCreationStrategy(EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached);
		EXPECT_EQ(Count(), 0u);
		ASSERT_TRUE(AddedMaterial->SetStaticProperties(Properties));
		ASSERT_TRUE(AddedMaterial->SetMaterialExpressions(Expressions, Outputs));
		FAssetCompilingManager::Get().FinishCompilationForObject(*AddedMaterial);
		Pump(1);
		auto* Spline = Cast<DSplineMeshComponent>(Actor->AddInstanceComponent(DSplineMeshComponent::StaticClass()));
		ASSERT_NE(Spline, nullptr); Spline->SetStaticMesh(Mesh);
		ASSERT_EQ(Spline->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().size(), 1u);
		EXPECT_EQ(Count(), 1u);
		Spline->UnregisterComponent();
		for (uint32 Index = 0; Index < 4; ++Index) DPrimitiveComponent::ProcessPSOPrecacheRequests(World.Get());
		EXPECT_EQ(Count(), 1u);
		Spline->RegisterComponent(); Pump(2);
		auto CheckParticipation = [&](bool bMain, bool bShadow) {
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
				FSceneView View;
				View.ViewportWidth = View.ViewportHeight = 64;
				for (const auto* Info : Scene.GetPrimitiveSceneInfos()) {
					EXPECT_EQ(Info->GetProxy().GetRenderParticipation().bRenderInMainPass, bMain);
					EXPECT_EQ(Info->GetProxy().GetRenderParticipation().bCastShadow, bShadow);
				}
				const auto Main = PrepareStaticMeshView_RenderThread(Commands, Scene.GetPrimitiveSceneInfos(), View,
					ERasterMode::Solid, ERenderPreparationMode::Full);
				const auto Shadow = PrepareStaticMeshView_RenderThread(Commands, Scene.GetPrimitiveSceneInfos(), View,
					ERasterMode::Solid, ERenderPreparationMode::ShadowDepth);
				EXPECT_EQ(Main.GetNumSections() != 0, bMain);
				EXPECT_EQ(Shadow.GetNumSections() != 0, bShadow);
				const auto CandidateCount = Scene.GetPrimitiveSceneInfos().size();
				EXPECT_EQ(Main.VisibleCandidates, CandidateCount);
				EXPECT_EQ(Shadow.VisibleCandidates, CandidateCount);
				if (!bMain) {
					EXPECT_EQ(Main.RejectedPrimitives, CandidateCount);
					EXPECT_EQ(Main.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Excluded)], CandidateCount);
				}
				if (!bShadow) {
					EXPECT_EQ(Shadow.RejectedPrimitives, CandidateCount);
					EXPECT_EQ(Shadow.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Excluded)], CandidateCount);
				}
			});
			FlushRenderingCommands();
		};
		const auto PreviousJobs = Component->GetPSOPrecacheComponentData().GetLatestJobSet();
		Component->SetCastShadow(false); Spline->SetCastShadow(false); Pump(2);
		EXPECT_GT(Component->GetPSOPrecacheComponentData().GetLatestJobSet(), PreviousJobs);
		CheckParticipation(true, false);
		const auto StableJobs = Component->GetPSOPrecacheComponentData().GetLatestJobSet();
		Component->SetCastShadow(false);
		EXPECT_EQ(Component->GetPSOPrecacheComponentData().GetLatestJobSet(), StableJobs);
		Component->SetRenderInMainPass(false); Spline->SetRenderInMainPass(false); Pump(2);
		EXPECT_TRUE(Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().empty());
		CheckParticipation(false, false);
		Component->SetCastShadow(true); Spline->SetCastShadow(true); Pump(2);
		CheckParticipation(false, true);
		for (const auto* Primitive : {static_cast<DPrimitiveComponent*>(Component), static_cast<DPrimitiveComponent*>(Spline)}) {
			FPSOPrecacheParams Params;
			Primitive->SetupPrecachePSOParams(Params);
			EXPECT_TRUE(Params.Participation.bCastShadow);
			EXPECT_FALSE(Params.Participation.bRenderInMainPass);
		}
		Component->SetRenderInMainPass(true); Spline->SetRenderInMainPass(true); Pump(2);
		CheckParticipation(true, true);
		Component->UnregisterComponent(); Spline->UnregisterComponent(); EXPECT_EQ(Count(), 0u);
	}
}

TEST(FStaticMeshRenderPreparationVulkanTests, HitProxyIdsRespectDepthBackgroundAndCacheInvalidation)
{
	using namespace Durin;
	if (!GIsGameThreadIdInitialized)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
	}
	InitializeDObjectSystem();
	ASSERT_EQ(GDynamicRHI, nullptr);
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("HitProxyGpuTest");
	Lifecycle.Start(Renderer);
	auto Data = MakeMultiLODRenderData();
	const auto Material = MakeMaterial(EMaterialBlendMode::Opaque, true);
	FSceneTestOwner Owner;
	auto& Scene = *Owner;
	EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([&](FRHICommandListImmediate& Commands) {
		ASSERT_TRUE(Data->InitResources(Commands));
	});
	FlushRenderingCommands();
	FSceneInterfaceTestAccess::AddPrimitiveProxy(Scene, FPrimitiveComponentId(501),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), FMatrix(1.0));
	FMatrix FarTransform(1.0);
	FarTransform[3][0] = 2.0;
	FSceneInterfaceTestAccess::AddPrimitiveProxy(Scene, FPrimitiveComponentId(502),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), FarTransform);
	FHitProxyRenderRequest Request;
	Request.View.ProjectionMatrix = FMatrix(0.0);
	Request.View.ProjectionMatrix[1][0] = 0.5;
	Request.View.ProjectionMatrix[2][1] = -0.5;
	Request.View.ProjectionMatrix[0][2] = 0.1;
	Request.View.ProjectionMatrix[3][2] = -0.1;
	Request.View.ProjectionMatrix[3][3] = 1.0;
	Request.View.ViewProjectionMatrix = Request.View.ProjectionMatrix;
	Request.View.ViewportWidth = Request.View.ViewportHeight = 64;
	Request.View.Settings.Mode.LODMode = EViewLODMode::ForceLOD0;
	Request.Primitives = {{501, {0xabcdef12}}, {502, {17}}};
	Request.Cache = std::make_shared<FHitProxyMapCache>();
	float CenterDistance = 0.0f;
	bool bSubmitDuplicate = true;
	std::shared_ptr<const FHitProxyMap> LastMap, DuplicateMap, SameFrameMap;
	const auto CaptureCenter = [&]() -> uint32 {
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		do
		{
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
				++GRenderFrameCounterRenderThread;
				GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
				LastMap = Renderer.RenderHitProxies(Commands, &Scene, Request);
				if (bSubmitDuplicate)
				{
					DuplicateMap = Renderer.RenderHitProxies(Commands, &Scene, Request);
					auto Uncached = Request;
					Uncached.Cache.reset();
					SameFrameMap = Renderer.RenderHitProxies(Commands, &Scene, Uncached);
				}

				GDynamicRHI->RHIEndFrame_RenderThread(Commands);
			});
			FlushRenderingCommands();
			if (LastMap && (!bSubmitDuplicate || (DuplicateMap && SameFrameMap))) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < Deadline);
		if (!LastMap) { ADD_FAILURE() << "Hit proxy preparation or readback timed out"; return 0; }
		FHitProxyPixel Center;
		if (!LastMap->ReadPixel(Request.View.ViewportX + Request.View.ViewportWidth / 2,
			Request.View.ViewportY + Request.View.ViewportHeight / 2, Center)) { ADD_FAILURE(); return 0; }
		CenterDistance = Center.Distance;
		EXPECT_TRUE(std::isfinite(CenterDistance));
		FHitProxyPixel Background;
		EXPECT_TRUE(LastMap->ReadPixel(0, 0, Background));
		EXPECT_EQ(Background.Id.Value, 0u);
		return Center.Id.Value;
	};
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	bSubmitDuplicate = false;
	const auto InitialMap = LastMap;
	ASSERT_TRUE(DuplicateMap);
	EXPECT_EQ(DuplicateMap, InitialMap);
	ASSERT_TRUE(SameFrameMap); // Further GPU work in this frame remains valid after the blocking readback.
	EXPECT_NE(SameFrameMap, InitialMap);
	FHitProxyPixel SameFramePixel;
	ASSERT_TRUE(SameFrameMap->ReadPixel(32, 32, SameFramePixel));
	EXPECT_EQ(SameFramePixel.Id.Value, 0xabcdef12u);
	const auto ReuseCount = Request.Cache->GetStatistics().ReuseCount;
	const auto RenderCount = Request.Cache->GetStatistics().RenderCount;
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_EQ(LastMap, InitialMap);
	EXPECT_EQ(Request.Cache->GetStatistics().RenderCount, RenderCount);
	EXPECT_EQ(Request.Cache->GetStatistics().ReuseCount, ReuseCount + 1);
	// An unrelated texture publication must not invalidate this scene's picking map.
	const auto BeforeTexturePublication = LastMap;
	EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([](FRHICommandListImmediate&) {
		FTextureReference Reference;
		const auto Texture = RHICreateTexture(FRHITextureCreateDesc::Create2D("HitProxyEpoch", 1, 1, EPixelFormat::RG32_UINT)
			.SetFlags(ETextureCreateFlags::ShaderResource));
		ASSERT_TRUE(Texture);
		Reference.SetReferencedTexture_RenderThread(Texture);
	});
	FlushRenderingCommands();
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_EQ(LastMap, BeforeTexturePublication);
	const auto UnrelatedMaterial = MakeMaterial(EMaterialBlendMode::Opaque);
	FlushRenderingCommands();
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_EQ(LastMap, BeforeTexturePublication);
	EnqueueRenderCommand("RepublishUsedHitProxyMaterial", [&](FRHICommandListImmediate&) {
		FMaterialResource Layer;
		Layer.CompiledProgram = Material->GetMaterialWithFallback().CompiledProgram;
		FMaterialStaticProperties Properties;
		Properties.bTwoSided = true;
		Layer.StaticProperties = Properties;
		Material->ApplyPublication_RenderThread(std::move(Layer));
	});
	FlushRenderingCommands();
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_NE(LastMap, BeforeTexturePublication);
	Request.View.ViewportWidth = Request.View.ViewportHeight = 32;
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_NE(LastMap, InitialMap);
	EXPECT_EQ(Request.Cache->GetStatistics().RenderCount, RenderCount + 2);
	Request.View.ViewportWidth = Request.View.ViewportHeight = 64;
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	const float OriginDistance = CenterDistance;
	const auto BeforeVisibilityChange = LastMap;
	Scene.UpdatePrimitiveVisibility(FPrimitiveComponentId(501), false);
	EXPECT_EQ(CaptureCenter(), 17u);
	EXPECT_NE(LastMap, BeforeVisibilityChange);
	FHitProxyPixel PreviousPixel;
	ASSERT_TRUE(BeforeVisibilityChange->ReadPixel(32, 32, PreviousPixel));
	EXPECT_EQ(PreviousPixel.Id.Value, 0xabcdef12u);
	Scene.UpdatePrimitiveVisibility(FPrimitiveComponentId(501), true);
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	// Two visible draws with the same material share one view cache, but never their request IDs.
	FMatrix SideTransform = FarTransform;
	SideTransform[3][1] = 2.0;
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(502),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), SideTransform);
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	FHitProxyPixel SidePixel;
	ASSERT_TRUE(LastMap->ReadPixel(56, 32, SidePixel));
	EXPECT_EQ(SidePixel.Id.Value, 17u);
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(502),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), FarTransform);
	const FVector3 LargeOrigin{1099511627776.0, -1099511627776.0, 1099511627776.0};
	const FMatrix LargeTranslation = Math::TranslationMatrix(LargeOrigin);
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(501),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), LargeTranslation);
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(502),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), LargeTranslation * FarTransform);
	Request.View.ViewLocation = LargeOrigin;
	Request.View.ViewMatrix = Math::TranslationMatrix(-LargeOrigin);
	Request.View.ViewProjectionMatrix = Request.View.ProjectionMatrix * Request.View.ViewMatrix;
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_NEAR(CenterDistance, OriginDistance, 1.0e-4f);
	// Restore the origin for the existing depth, mask, spline, and invalid-output cases.
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(501),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), FMatrix(1.0));
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(502),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}), FarTransform);
	Request.View.ViewLocation = FVector3(0.0);
	Request.View.ViewMatrix = FMatrix(1.0);
	Request.View.ViewProjectionMatrix = Request.View.ProjectionMatrix;

	FHitProxyOverlay Handle;
	Handle.Id = FHitProxyId{91};
	const std::array<FVector4f, 4> Corners{FVector4f(-.2f,-.2f,.8f,1.f), FVector4f(.2f,-.2f,.8f,1.f),
		FVector4f(.2f,.2f,.8f,1.f), FVector4f(-.2f,.2f,.8f,1.f)};
	constexpr uint32 Indices[]{0,1,2,0,2,3};
	for (uint32 I=0; I<6; ++I) Handle.Vertices[I] = {Corners[Indices[I]], 9.f};
	Request.Overlays = {Handle};
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u); // A depth-tested handle behind the mesh is occluded.
	Request.Overlays.front().bForeground = true;
	EXPECT_EQ(CaptureCenter(), 91u); // Foreground handles bypass scene depth.
	Request.Overlays.front().Priority = 100;
	Handle.bForeground = true;
	Handle.Id = FHitProxyId{92};
	Request.Overlays.push_back(Handle);
	EXPECT_EQ(CaptureCenter(), 91u); // Priority, not submission order, chooses the front handle.
	Request.Overlays.clear();
	Request.Primitives.erase(Request.Primitives.begin());
	EXPECT_EQ(CaptureCenter(), 0u); // Unselectable foreground geometry still occludes.
	Request.Primitives.insert(Request.Primitives.begin(), {501, {0xabcdef12}});

	Request.View.DepthConvention = ESceneDepthConvention::ReversedZ;
	Request.View.ProjectionMatrix[0][2] = -.1;
	Request.View.ProjectionMatrix[3][2] = 1.1;
	Request.View.ViewProjectionMatrix = Request.View.ProjectionMatrix;
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	auto* Masked = NewObject<DMaterial>(nullptr, "HitProxyMasked");
	auto* Mask = NewObject<DMaterialExpressionScalarConstant>(Masked, "Mask");
	Mask->Id = FGuid::NewGuid();
	Mask->Value = 0.f;
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.OpacityMask = {Mask->Id};
	const std::array<DMaterialExpression*, 1> Expressions{Mask};
	EXPECT_TRUE(Masked->SetMaterialExpressions(Expressions, Outputs));
	FMaterialStaticProperties Properties;
	Properties.BlendMode = EMaterialBlendMode::Masked;
	Properties.bTwoSided = true;
	EXPECT_TRUE(Masked->SetStaticProperties(Properties));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Masked);
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(501),
		std::make_unique<FStaticMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Masked->GetRenderProxy()}), FMatrix(1.0));
	EXPECT_EQ(CaptureCenter(), 17u); // Masked-out fragments reveal the far surface.
	FSplineMeshRenderDynamicData Spline;
	Spline.Params.SourceForwardMax = 5.0;
	Spline.LocalBounds = Data->LocalBounds;
	FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, FPrimitiveComponentId(501),
		std::make_unique<FSplineMeshSceneProxy>(Data.get(), std::vector<FMaterialRenderProxyRef>{Material}, Spline), FMatrix(1.0));
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	const auto BeforeInvalidOutput = Request.Cache->GetStatistics().RenderCount;
	const auto BeforeInvalidMap = LastMap;
	Request.View.ViewportWidth = 0;
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
		LastMap = Renderer.RenderHitProxies(Commands, &Scene, Request);
	});
	FlushRenderingCommands();
	EXPECT_FALSE(LastMap);
	EXPECT_EQ(Request.Cache->GetStatistics().RenderCount, BeforeInvalidOutput);
	Request.View.ViewportWidth = 64;
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_EQ(LastMap, BeforeInvalidMap);
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) {
		Request.Cache->Invalidate_RenderThread();
	});
	FlushRenderingCommands();
	EXPECT_EQ(CaptureCenter(), 0xabcdef12u);
	EXPECT_NE(LastMap, BeforeInvalidMap);
	EXPECT_EQ(Request.Cache->GetStatistics().RenderCount, BeforeInvalidOutput + 1);
	FHitProxyPixel RetiredPixel;
	ASSERT_TRUE(BeforeInvalidMap->ReadPixel(32, 32, RetiredPixel));
	EXPECT_EQ(RetiredPixel.Id.Value, 0xabcdef12u);
	Owner.Reset();
	EnqueueRenderCommand<FInitializePreparedStaticMeshResourcesCommand>([&](FRHICommandListImmediate&) { Data->ReleaseResources(); });
	FlushRenderingCommands();
	Lifecycle.Shutdown();
	FlushRenderingCommands();
	ShutdownRenderingThread();
	FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
	RHIExit();
}

TEST(FStaticMeshRenderPreparationVulkanTests, SharedFactsRejectChangedTransformsAndMaterialResources)
{
	using namespace Durin;
	FStaticMeshPreparationCache Cache;
	const FPrimitiveComponentId Id(1);
	const FMatrix Transform = Math::ScaleMatrix(FVector3(-2.0, 3.0, 4.0));
	EXPECT_TRUE(Cache.ResolveTransform(Id, 1, Transform).bValid);
	EXPECT_LT(Cache.ResolveTransform(Id, 1, Transform).Determinant, 0.0);
	EXPECT_EQ(Cache.TransformBuilds, 1u);
	EXPECT_TRUE(Cache.ResolveTransform(Id, 2, FMatrix(1.0)).bValid);
	EXPECT_EQ(Cache.TransformBuilds, 2u);
	EXPECT_FALSE(Cache.ResolveTransform(Id, 1, Math::ScaleMatrix(FVector3(0.0))).bValid);
	EXPECT_TRUE(Cache.ResolveTransform(Id, 1, Transform).bValid);
	EXPECT_EQ(Cache.TransformBuilds, 4u);

	const FGuid Scalar = FGuid::NewGuid();
	const FGuid Texture = FGuid::NewGuid();
	const std::array Declarations{
		FMaterialCompilerParameterDeclaration{Scalar, EMaterialParameterType::Scalar},
		FMaterialCompilerParameterDeclaration{Texture, EMaterialParameterType::Texture}};
	const auto Layout = CompileMaterialLayout(Declarations);
	ASSERT_TRUE(Layout);
	FMaterialRenderRepresentationBuilder Builder(Layout.Layout);
	ASSERT_TRUE(Builder.SetScalar(Scalar, 1.0f));
	ASSERT_TRUE(Builder.SetTexture(Texture, {}, {}, EMaterialTextureFallback::White));
	FMaterialRenderData First;
	FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Builder.Build(First.Representation, Diagnostic));
	const FMaterialUniformSortKey FirstSortKey{First.Representation};
	EXPECT_EQ(FirstSortKey.Representation.GetUniformPayload().data(), First.Representation.GetUniformPayload().data());
	const auto FirstIndex = Cache.ResolveMaterial(First);
	ASSERT_TRUE(FirstIndex);
	FMaterialRenderData Copy = First;
	EXPECT_EQ(Cache.ResolveMaterial(Copy), FirstIndex);
	EXPECT_EQ(Cache.MaterialBuilds, 1u);
	ASSERT_TRUE(Builder.SetTexture(Texture, {}, {}, EMaterialTextureFallback::Black));
	FMaterialRenderData Other;
	ASSERT_TRUE(Builder.Build(Other.Representation, Diagnostic));
	const FMaterialUniformSortKey EqualUniformKey{Other.Representation};
	EXPECT_NE(FirstSortKey.Representation.GetRecordId(), EqualUniformKey.Representation.GetRecordId());
	EXPECT_EQ(FirstSortKey, EqualUniformKey);
	const auto OtherIndex = Cache.ResolveMaterial(Other);
	ASSERT_TRUE(OtherIndex);
	EXPECT_NE(OtherIndex, FirstIndex);
	ASSERT_TRUE(Builder.SetScalar(Scalar, 2.0f));
	ASSERT_TRUE(Builder.Build(Other.Representation, Diagnostic));
	const FMaterialUniformSortKey ChangedSortKey{Other.Representation};
	EXPECT_NE(FirstSortKey, ChangedSortKey);
	EXPECT_EQ(FirstSortKey, EqualUniformKey);
	const auto ChangedIndex = Cache.ResolveMaterial(Other);
	EXPECT_NE(ChangedIndex, OtherIndex);
	EXPECT_NE(ChangedIndex, FirstIndex);
	EXPECT_EQ(Cache.MaterialBuilds, 3u);
}

TEST(FStaticMeshRenderPreparationVulkanTests, RepeatedVulkanInitializationPreservesDiagnostics)
{
	using namespace Durin;
	FModuleManager::Get().LoadModule("RenderCore");
	std::optional<bool> ValidationActive;
	for (uint32 Cycle = 0; Cycle < 8; ++Cycle)
	{
		SCOPED_TRACE(Cycle);
		ASSERT_EQ(GDynamicRHI, nullptr);
		RHIInit(Tests::GetVulkanEngineTestInitializationContext());
		ASSERT_NE(GDynamicRHI, nullptr);
		bool Active = false;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
			Active = GDynamicRHI->RHIGetDiagnosticSnapshot().Availability.bValidationLayerActive;
		});
		if (ValidationActive) EXPECT_EQ(Active, *ValidationActive);
		else ValidationActive = Active;
		RHIExit();
		EXPECT_EQ(GDynamicRHI, nullptr);
	}
}

TEST(FStaticMeshRenderPreparationVulkanTests, AuthoredLODsInitializeAllResourcesAndCollisionSelectionPreservesGeometryRecords)
{
	using namespace Durin;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) InitializeAssetCompilingManager();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FMeshDescription Primary;
	Primary.PolygonGroups.push_back({"Default", 0, "Default"});
	auto& Section = Primary.Sections.emplace_back();
	Section.Name = "Primary";
	Section.Positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
	Section.Indices = {0, 1, 2, 0, 2, 3};
	auto Lower = Primary;
	Lower.Sections.front().Indices = {0, 1, 2};
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.InitializeLODs({std::move(Primary), std::move(Lower)}));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, "AuthoredMultiLODVulkanMesh");
	const auto Built = Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous,
		{.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(Source)});
	ASSERT_TRUE(Built) << Built.error();
	ASSERT_EQ(GDynamicRHI, nullptr);
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	Mesh->InitResources();
	FlushRenderingCommands();
	const auto* Render = Mesh->GetRenderData();
	ASSERT_NE(Render, nullptr);
	ASSERT_EQ(Render->LODResources.size(), 2u);
	ASSERT_TRUE(Render->IsReadyForRendering());
	std::array<uint64, 2> Records;
	for (uint32 LOD = 0; LOD < 2; ++LOD)
	{
		ASSERT_TRUE(Render->LODResources[LOD].GeometryRecord);
		Records[LOD] = Render->LODResources[LOD].GeometryRecord->GetRecordId();
		EXPECT_NE(Records[LOD], 0u);
	}
	const auto RenderRevision = Mesh->GetRenderResourceStatus().Revision;
	Mesh->SetLODForCollision(1);
	FlushRenderingCommands();
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, RenderRevision);
	EXPECT_EQ(Mesh->GetPhysicsTriMeshData()->Indices.size(), 3u);
	for (uint32 LOD = 0; LOD < 2; ++LOD)
		EXPECT_EQ(Render->LODResources[LOD].GeometryRecord->GetRecordId(), Records[LOD]);
	MarkAsGarbage(Mesh);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	FlushRenderingCommands();
	ShutdownRenderingThread();
	RHIExit();
}

namespace
{
	using Durin::RendererPrivate::FMeshPassDrawListContext;
	using Durin::RendererPrivate::FMeshDrawCommand;
	using Durin::RendererPrivate::FDynamicPassMeshDrawListContext;
	class FObservedCommandList final : public FDynamicPassMeshDrawListContext
	{
	public:
		using FDynamicPassMeshDrawListContext::FDynamicPassMeshDrawListContext;
		auto IsCached() const -> bool override { return false; }
		auto AddCommand(const FMeshDrawCommand& Initializer) -> FMeshDrawCommand& override
		{ ++Added; return FMeshPassDrawListContext::AddCommand(Initializer); }
		auto FinalizeCommand() -> void override
		{ ++Finalized; FDynamicPassMeshDrawListContext::FinalizeCommand(); }
		uint32 Added = 0, Finalized = 0;
	};
}

TEST(FStaticMeshRenderPreparationVulkanTests, SceneFeatureLevelMatchesComponentMaterialAndPSOSelection)
{
	Durin::Tests::FScopedComponentPSOPrecacheMode PrecacheMode(Durin::EComponentPSOPrecacheMode::Enabled);
	using namespace Durin; using namespace Durin::Tests;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) InitializeAssetCompilingManager();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints()); ASSERT_TRUE(InitializeAssetRuntime()); ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext()); ASSERT_NE(GDynamicRHI, nullptr); InitRenderingThread();
	FRendererModule Renderer; FModuleTestHarness Lifecycle("SceneFeatureLevelIntegration"); Lifecycle.Start(Renderer);
	struct FRendererCleanup {
		FModuleTestHarness& Lifecycle;
		~FRendererCleanup() { Lifecycle.Shutdown(); ShutdownRenderingThread(); RHIExit(); }
	} RendererCleanup{Lifecycle};
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Low(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionScalarParameter> High(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialExpressionFeatureLevelSwitch> Select(NewObject<DMaterialExpressionFeatureLevelSwitch>(nullptr, NAME_None));
	Low->Id = FGuid::NewGuid(); Low->Metadata = {.Id = FGuid::NewGuid(), .Name = "LowSceneValue"}; Low->DefaultValue = 0.25f;
	High->Id = FGuid::NewGuid(); High->Metadata = {.Id = FGuid::NewGuid(), .Name = "HighSceneValue"}; High->DefaultValue = 0.75f;
	Select->Id = FGuid::NewGuid(); Select->DefaultValue.Connection = {Low->Id}; Select->SM5 = {High->Id}; Select->SM6 = {High->Id};
	FMaterialExpressionSurfaceOutputs Outputs; Outputs.Roughness = {Select->Id};
	const std::array<DMaterialExpression*, 3> Expressions{Low.Get(), High.Get(), Select.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
	const auto DefaultProgram = Material->GetAcceptedCompiledProgram(); ASSERT_TRUE(DefaultProgram);
	auto HighScene = Renderer.CreateScene();
	auto LowScene = Renderer.CreateScene(ERHIFeatureLevel::ES3_1);
	ASSERT_TRUE(HighScene && LowScene);
	EXPECT_EQ(HighScene->GetFeatureLevel(), GDynamicRHI->RHIGetCapabilities()->FeatureLevel);
	EXPECT_EQ(LowScene->GetFeatureLevel(), ERHIFeatureLevel::ES3_1);
	EXPECT_FALSE(Renderer.CreateScene(static_cast<ERHIFeatureLevel>(255)));
	TStrongObjectPtr<DWorld> HighWorld(NewObject<DWorld>(nullptr, NAME_None)), LowWorld(NewObject<DWorld>(nullptr, NAME_None));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, NAME_None);
	struct FSceneCleanup {
		DWorld* High; DWorld* Low; FScenePtr& HighScene; FScenePtr& LowScene; DStaticMesh* Mesh;
		~FSceneCleanup() {
			High->CleanupWorld();
			Low->CleanupWorld();
			HighScene->Release(); LowScene->Release(); HighScene.reset(); LowScene.reset(); FlushRenderingCommands();
			auto* Data = FStaticMeshTestAccess::GetMutableRenderData(Mesh);
			if (Data) EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([Data](FRHICommandListImmediate&) { Data->ReleaseResources(); });
			FlushRenderingCommands(); MarkAsGarbage(Mesh); CollectGarbage(Durin::EObjectFlags::Standalone); FlushRenderingCommands();
		}
	} Cleanup{HighWorld.Get(), LowWorld.Get(), HighScene, LowScene, Mesh};
	std::vector<FMeshMaterialSlotDefinition> Slots;
	for (uint32 Slot = 0; Slot < 4; ++Slot) Slots.push_back({.Name = FName(std::format("FeatureSlot{}", Slot)), .DefaultMaterial = Material.Get()});
	ASSERT_TRUE(FStaticMeshTestAccess::ReplaceRenderData(Mesh, MakeRenderData(), std::move(Slots)));
	auto AddComponent = [&](DWorld& World, FSceneInterface& Scene) -> DStaticMeshComponent* {
		World.InitializeSubsystems();
		auto* Level = NewObject<DLevel>(&World, NAME_None);
		if (!World.SetCurrentLevel(Level)) return nullptr;
		World.SetRenderScene(&Scene);
		auto* Actor = Level->SpawnActor<AActor>();
		auto* Component = Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(DStaticMeshComponent::StaticClass()));
		Component->SetStaticMesh(Mesh);
		return Component;
	};
	auto* HighComponent = AddComponent(*HighWorld, *HighScene);
	auto* LowComponent = AddComponent(*LowWorld, *LowScene);
	ASSERT_TRUE(HighComponent && LowComponent);
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	bool Ready = false;
	while (!Ready && std::chrono::steady_clock::now() < Deadline) {
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			++GRenderFrameCounterRenderThread; Renderer.BeginFrame_RenderThread(Commands);
		});
		FlushRenderingCommands(); DPrimitiveComponent::ProcessPSOPrecacheRequests(HighWorld.Get()); DPrimitiveComponent::ProcessPSOPrecacheRequests(LowWorld.Get());
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate&) {
			Ready = static_cast<FScene*>(HighScene.get())->GetPrimitiveSceneInfos().size() == 1
				&& static_cast<FScene*>(LowScene.get())->GetPrimitiveSceneInfos().size() == 1;
		});
		FlushRenderingCommands(); if (!Ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(Ready) << HighComponent->GetPSOPrecacheDiagnostic() << "; " << LowComponent->GetPSOPrecacheDiagnostic();
	const auto HighResource = Material->GetMaterialResource(HighScene->GetFeatureLevel());
	const auto LowResource = Material->GetMaterialResource(LowScene->GetFeatureLevel());
	ASSERT_TRUE(HighResource.HasValidGameThreadShaderMap() && LowResource.HasValidGameThreadShaderMap());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram(), DefaultProgram);
	EXPECT_EQ(LowResource.CompiledProgram->FeatureLevel, ERHIFeatureLevel::ES3_1);
	if (HighScene->GetFeatureLevel() != ERHIFeatureLevel::ES3_1)
		EXPECT_NE(HighResource.CompiledProgram->GetShaderMap().ShaderMapId, LowResource.CompiledProgram->GetShaderMap().ShaderMapId);
	const auto Factory = RendererPrivate::FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey()); ASSERT_TRUE(Factory);
	const std::array Factories{FPSOPrecacheVertexFactoryData(&Factory->GetType())};
	std::vector<FMaterialPSOPrecacheRequestID> Repeated;
	ASSERT_TRUE(Material->PrecachePSOs(Renderer, ERHIFeatureLevel::ES3_1, Factories, {}, EPSOPrecachePriority::Medium, Repeated));
	ASSERT_EQ(Repeated.size(), 1u);
	ASSERT_EQ(LowComponent->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs().size(), 1u);
	EXPECT_EQ(Repeated[0].GetValue(), LowComponent->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs()[0].GetValue());
	{
		class FPrecacheHost final : public DEngine
		{
		public:
			explicit FPrecacheHost(IRendererModule& Module) : DEngine(FObjectInitializer::Get()) { RendererModule = &Module; }
		} Host(Renderer);
		auto* PreviousEngine = std::exchange(GEngine, &Host);
		struct FRestoreHost { DEngine* Previous; ~FRestoreHost() { GEngine = Previous; } } RestoreHost{PreviousEngine};
		std::vector<FMaterialPSOPrecacheRequestID> ViaHost;
		ASSERT_TRUE(Material->PrecachePSOs(ERHIFeatureLevel::ES3_1, Factories, {}, EPSOPrecachePriority::Medium, ViaHost));
		ASSERT_EQ(ViaHost.size(), 1u);
		EXPECT_EQ(ViaHost[0].GetValue(), Repeated[0].GetValue());
		if (HighScene->GetFeatureLevel() != ERHIFeatureLevel::ES3_1)
		{
			const auto HighRequests = HighComponent->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs();
			ASSERT_EQ(HighRequests.size(), 1u);
			EXPECT_NE(ViaHost[0].GetValue(), HighRequests[0].GetValue());
		}
		// Rejection must preserve the caller's existing observations.
		GEngine = nullptr;
		EXPECT_FALSE(Material->PrecachePSOs(ERHIFeatureLevel::ES3_1, Factories, {}, EPSOPrecachePriority::Medium, ViaHost));
		ASSERT_EQ(ViaHost.size(), 1u);
		EXPECT_EQ(ViaHost[0].GetValue(), Repeated[0].GetValue());
		EXPECT_TRUE(ViaHost[0].IsCurrent());
		EXPECT_FALSE(Material->PrecachePSOs(Renderer, static_cast<ERHIFeatureLevel>(255), Factories, {}, EPSOPrecachePriority::Medium, ViaHost));
		ASSERT_EQ(ViaHost.size(), 1u);
		EXPECT_EQ(ViaHost[0].GetValue(), Repeated[0].GetValue());
		EXPECT_TRUE(ViaHost[0].IsCurrent());
	}
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
		for (auto* Scene : {static_cast<FScene*>(HighScene.get()), static_cast<FScene*>(LowScene.get())}) {
			FSceneView View; View.ViewportWidth = View.ViewportHeight = 64; View.FeatureLevel = Scene->GetFeatureLevel();
			const auto Prepared = PrepareStaticMeshView_RenderThread(Commands, Scene->GetPrimitiveSceneInfos(), View, ERasterMode::Solid);
			ASSERT_GT(Prepared.GetNumSections(), 0u);
			for (const auto& Draw : Prepared.Opaque) {
				ASSERT_TRUE(Draw.Command->Material.CompiledProgram);
				EXPECT_EQ(Draw.Command->Material.CompiledProgram->FeatureLevel, Scene->GetFeatureLevel());
			}
		}
	});
	FlushRenderingCommands();
	const auto InitialConfig = Renderer.GetSceneRenderPreparationConfig(LowScene->GetFeatureLevel());
	ASSERT_NE(InitialConfig.Version, 0u);
	EXPECT_EQ(InitialConfig.FeatureLevel, LowScene->GetFeatureLevel());
	EXPECT_EQ(InitialConfig.bSupportsWireframe, GDynamicRHI->RHIGetCapabilities()->bSupportsNonSolidFill);
	ASSERT_TRUE(Renderer.SetPreparationSettings(InitialConfig.Settings));
	EXPECT_EQ(Renderer.GetSceneRenderPreparationConfig(LowScene->GetFeatureLevel()), InitialConfig);
	std::unique_ptr<RendererPrivate::FMeshPassProcessor> DirectCollector;
	FMaterialPSOPrecacheParams DirectParams;
	FRenderPipelineRequests DirectCommandRequests;
	FGraphicsPipelineStateRef ExpectedCommandPipeline;
	FPSOPrecacheDataArray DirectInitializers;
	DirectInitializers.emplace_back(); // The append interface must preserve its prefix.
	struct FDirectCollectorCleanup {
		std::unique_ptr<RendererPrivate::FMeshPassProcessor>& Processor;
		FPSOPrecacheDataArray& Initializers;
		FRenderPipelineRequests& Requests;
		FGraphicsPipelineStateRef& Expected;
		~FDirectCollectorCleanup() {
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([this](FRHICommandListImmediate&) { Initializers.clear(); Processor.reset(); Requests.Reset(); Expected.reset(); });
			FlushRenderingCommands();
		}
	} DirectCleanup{DirectCollector, DirectInitializers, DirectCommandRequests, ExpectedCommandPipeline};
	bool DirectReady = false, CommandReady = false, BindingContextsValidated = false;
	std::string DirectError;
	const auto DirectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	do {
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			FGeometryQualificationFrame Frame(Commands);
			if (!DirectCollector) {
				DirectCollector = RendererPrivate::FPassProcessorManager::CreateMeshPassProcessor(RendererPrivate::EMeshPipelinePass::Forward);
				const auto Properties = LowResource.GetStaticProperties();
				const auto& Map = LowResource.CompiledProgram->GetShaderMap();
				FMaterialPSOPrecacheParams Params;
				Params.Material = {.ShaderMap = {.RenderLayout = Map.Layout.Identity, .ProgramIdentity = Map.ShaderMapId,
					.BlendMode = Properties.BlendMode, .ShadingModel = Properties.ShadingModel, .OpacityMaskThreshold = Properties.OpacityMaskThreshold},
					.bTwoSided = Properties.bTwoSided, .DepthWritePolicy = Properties.DepthWritePolicy};
				Params.CompiledProgram = LowResource.CompiledProgram; Params.VertexFactoryData = Factories.front();
				Params.FeatureLevel = InitialConfig.FeatureLevel; Params.PreparationConfig = InitialConfig; Params.Priority = EPSOPrecachePriority::High;
				DirectParams = Params;
			}
			const auto Draws = Commands.GetNumRecordedDrawCommands();
			const auto Result = DirectCollector->PreparePSOResources(DirectParams, Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation, false);
			if (!Result) { DirectError = Result.error(); return; }
			DirectReady = *Result == EPSOCollectorPreparation::Ready;
			if (DirectReady) {
				if (DirectInitializers.size() == 1) DirectCollector->CollectPSOInitializers(InitialConfig, LowResource, DirectParams.VertexFactoryData, {}, DirectInitializers);

				using namespace RendererPrivate;
				const auto Properties = LowResource.GetStaticProperties();
				const auto& Map = LowResource.CompiledProgram->GetShaderMap();
				FMaterialPlanningPassIdentity Identity{
					.ShaderMap = {.RenderLayout = Map.Layout.Identity, .ProgramIdentity = Map.ShaderMapId,
						.BlendMode = Properties.BlendMode, .ShadingModel = Properties.ShadingModel,
						.OpacityMaskThreshold = Properties.OpacityMaskThreshold},
					.bTwoSided = Properties.bTwoSided, .DepthWritePolicy = Properties.DepthWritePolicy};
				const auto Raster = DirectCollector->GetPSOPrecacheVariants(InitialConfig, {}).front().Raster;
				FMeshPassProcessorBatch Batch{Identity, *Factory, MakeMeshPipelineState(Identity, Raster),
					EGeometryTopology::TriangleList, {}, true, Raster.bReversedZ};
				FMeshPassShaders Shaders;
				const auto Generation = Renderer.GetResourceInvalidationSnapshot_RenderThread().Generation;
				const auto ShaderContext = DirectCollector->PrepareShaders(Batch, Generation, LowResource.CompiledProgram.get(), {}, Shaders);
				ASSERT_TRUE(ShaderContext) << ShaderContext.Error;
				ASSERT_NE(Shaders.FragmentLayout, nullptr);
				const auto& Initializer = DirectInitializers[1].GetGraphicsInitializer();
				if (!ExpectedCommandPipeline) {
					const auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(Initializer, "ExpectedPrecachedMeshCommand");
					ASSERT_TRUE(Request); ExpectedCommandPipeline = *Request;
				}
				FRenderPipelineRequestScope Requests(DirectCommandRequests, Generation);
				FMeshPassPipeline Command;
				const auto DrawContext = DirectCollector->PreparePipeline(Batch, "AddMeshBatchCommand", Shaders, Initializer.VertexDeclaration, Command);
				CommandReady = static_cast<bool>(DrawContext);
				if (CommandReady) {
					EXPECT_EQ(Command.PipelineState, ExpectedCommandPipeline->GetRHIPipeline());
					EXPECT_EQ(Command.VertexShader->GetRHIShader(false), Initializer.BoundShaders.VertexShader);
					EXPECT_EQ(Command.FragmentShader, Initializer.BoundShaders.FragmentShader);
					EXPECT_EQ(Command.VertexDeclaration, Initializer.VertexDeclaration);
				}
				else EXPECT_TRUE(Requests.HasPending()) << DrawContext.Error;
				FGraphicsPipelineStateInitializer Rejected;
				Identity.ShaderMap.BlendMode = EMaterialBlendMode::Translucent;
				const auto WrongMaterial = DirectCollector->BuildPSOInitializer(Batch, Shaders, Initializer.VertexDeclaration, Rejected);
				EXPECT_EQ(WrongMaterial.Status, EShaderBuildStatus::Error);
				EXPECT_EQ(Rejected.BoundShaders.VertexShader, nullptr);
				EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), Draws);
				if (CommandReady && !BindingContextsValidated) {
					PSOPrecacheValidation::SetEnabled(true);
					struct FValidationCleanup { ~FValidationCleanup() { PSOPrecacheValidation::SetEnabled(false); } } ValidationCleanup;
					FSceneView BindingView; BindingView.ViewportWidth = BindingView.ViewportHeight = 64;
					BindingView.FeatureLevel = LowScene->GetFeatureLevel();
					const auto Prepared = PrepareStaticMeshView_RenderThread(Commands, static_cast<FScene*>(LowScene.get())->GetPrimitiveSceneInfos(), BindingView, ERasterMode::Solid);
					ASSERT_FALSE(Prepared.Opaque.empty());
					auto BindingDraw = Prepared.Opaque.front();
					const auto* Primitive = Prepared.GetPrimitive(BindingDraw); ASSERT_TRUE(Primitive && Primitive->CollectedBinding);
					const auto& PassProcessor = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::ShadowDepth);
					const FMeshPassProcessorBatch ShadowBatch{BindingDraw.Command->Material.PlanningPassIdentity, *Factory,
						{BindingDraw.Command->PipelineKey.Rasterizer, BindingDraw.Command->PipelineKey.Depth, BindingDraw.Command->PipelineKey.ColorBlend},
						BindingDraw.Command->PipelineKey.Topology, {}, BindingDraw.Command->bSupportsGBuffer};
					FMeshPassShaders ShadowShaders;
					const auto ShaderContext = PassProcessor.PrepareShaders(ShadowBatch, Generation, LowResource.CompiledProgram.get(), {}, ShadowShaders);
					if (ShaderContext.Status == EShaderBuildStatus::Pending) { CommandReady = false; return; }
					ASSERT_TRUE(ShaderContext) << ShaderContext.Error;
					auto Pipeline = std::make_shared<FMeshPassPipeline>();
					const auto PipelineContext = PassProcessor.PreparePipeline(ShadowBatch, "BindingContextShadow", ShadowShaders, Primitive->CollectedBinding->Declaration, *Pipeline);
					if (!PipelineContext && Requests.HasPending()) { CommandReady = false; return; }
					ASSERT_TRUE(PipelineContext) << PipelineContext.Error;
					EXPECT_EQ(PSOPrecacheValidation::GetStatistics().Missed, 0u);
					EXPECT_EQ(PSOPrecacheValidation::GetStatistics().TooLate, 0u);
					const auto ValidationInitializer = Pipeline->Description.MakeInitializer(ShadowShaders.VertexShader->GetRHIShader(false),
						ShadowShaders.FragmentShader, Pipeline->VertexDeclaration, ShadowShaders.ShaderMap.GetPipelineLayout());
					FPSOPrecacheData ValidationData(ValidationInitializer);
					ValidationData.PSOCollectorIndex = FPSOCollectorCreateManager::GetIndex(EShadingPath::Deferred, "ShadowDepth");
					ValidationData.VertexFactoryType = &Factory->GetType();
					const auto ValidationNative = PipelineStateCache::PrecacheGraphicsPipelineState(ValidationInitializer, "MeshSubmissionValidation");
					ASSERT_TRUE(ValidationNative);
					ASSERT_EQ((*ValidationNative)->GetState(), ERHIPipelineRequestState::Ready);
					PSOPrecacheValidation::Observe(PSOPrecacheValidation::Track(ValidationData), (*ValidationNative)->GetStateObservation());

					FMaterialRenderBinding Binding;
					FMaterialRenderValidationDiagnostic BindingDiagnostic;
					ASSERT_TRUE(TryGetMaterialRenderBinding(BindingDraw.Command->Material.Representation, Binding, BindingDiagnostic));
					FResolvedStaticMeshView::FMaterialUniform PassMaterial;
					PassMaterial.Surface.bCompiledLayout = true; PassMaterial.Surface.bError = Binding.bError;
					PassMaterial.Uniform = Commands.CreateUniformBufferRange(Binding.CompiledUniformPayload.data(), static_cast<uint32>(Binding.CompiledUniformPayload.size()));
					const auto Transform = BuildFallbackPrimitiveUniform(*Primitive);
					const auto PrimitiveUniform = Commands.CreateUniformBufferRange(&Transform, sizeof(Transform));
					FMaterialViewUniform ViewParameters;
					ViewParameters.Viewport = FVector4f(0, 0, 64, 64);
					FMeshDrawCommandBuildInput Input{Commands, *Primitive, BindingDraw, Pipeline, PassMaterial,
						PrimitiveUniform, Commands.CreateUniformBufferRange(&ViewParameters, sizeof(ViewParameters)), {}, {}};
					FRendererResourceCoordinator UploadCoordinator;
                    FRendererRDGAllocator UploadAllocator(UploadCoordinator);
                    const auto SceneData = std::make_shared<FGPUSceneDynamicContext>()->Prepare(Commands, UploadAllocator, Prepared);
					ASSERT_EQ(SceneData.size(), Prepared.Primitives.size());
					ASSERT_TRUE(SceneData[BindingDraw.PrimitiveIndex].Buffers);
					Input.GPUScene = SceneData[BindingDraw.PrimitiveIndex].Buffers->GetShaderParameters();
					Input.InstanceIdOffset = SceneData[BindingDraw.PrimitiveIndex].InstanceIdOffset;
					FMeshDrawCommandBindingCache CacheA;
					std::shared_ptr<const FMeshDrawCommand> Cached;
					const auto Source = Primitive->SceneInfo.Resolve(); ASSERT_TRUE(Source);
					auto& SourceUniform = Source->GetPrimitiveUniformBuffer(Primitive->BatchId);
					ASSERT_TRUE(SourceUniform.Update(Commands, std::as_bytes(std::span(&Transform, 1))));
					Input.PrimitiveUniform = SourceUniform.GetRange();
					std::shared_ptr<const FMeshDrawCommand> DynamicA, DynamicB;
					FCachedMeshDrawCommandStorage Persistent;
					FDynamicMeshDrawCommandStorage FrameA, FrameB;
					FMeshDrawCommandHandle DynamicAHandle, DynamicBHandle;
					FCachedPassMeshDrawListContext CachedContext(Persistent, {EMeshPipelinePass::ShadowDepth}, Cached, Input); const auto CachedContextResult = PassProcessor.AddMeshBatch(ShadowBatch, Input, CacheA, CachedContext);
					ASSERT_TRUE(CachedContextResult) << CachedContextResult.Error;
					ASSERT_TRUE(Cached && Cached->bCached); EXPECT_EQ(Cached->Pipeline->Pass, EMeshPipelinePass::ShadowDepth);
					EXPECT_FALSE(Cached->VertexPassBindings); EXPECT_FALSE(Cached->FragmentPassBindings);
					EXPECT_FALSE(Cached->SubmitDraw(Commands));
					ASSERT_TRUE(Cached->bFinalized);
					EXPECT_EQ(Cached->VertexStreams.size(), Primitive->CollectedBinding->Streams.size());
					EXPECT_EQ(Cached->IndexBuffer, BindingDraw.Command->Indices.Buffer);
					EXPECT_EQ(std::get<FRHIDrawIndexedArguments>(Cached->DrawParameters), BindingDraw.Command->Geometry.GetIndexedDrawArguments());
					BindingDraw.RasterBias = {1.0f, 2.0f, 0.0f};
					FDynamicPassMeshDrawListContext DynamicContextA(FrameA, {EMeshPipelinePass::ShadowDepth}, DynamicAHandle); const auto DynamicContextAResult = PassProcessor.AddMeshBatch(ShadowBatch, Input, CacheA, DynamicContextA, Cached);
					ASSERT_TRUE(DynamicContextAResult) << DynamicContextAResult.Error; DynamicA = FrameA.GetCommand(DynamicAHandle);
					EXPECT_EQ(Persistent.Num(), 1u);
					EXPECT_EQ(FrameA.Num({EMeshPipelinePass::ShadowDepth}), 1u);
					ASSERT_TRUE(DynamicA && !DynamicA->bCached && DynamicA->VertexPassBindings);
					EXPECT_EQ(DynamicA->VertexBindings, Cached->VertexBindings);
					EXPECT_EQ(DynamicA->VertexStreams.size(), Cached->VertexStreams.size());
					EXPECT_EQ(DynamicA->IndexBuffer, Cached->IndexBuffer);
					EXPECT_EQ(DynamicA->DepthBias, *BindingDraw.RasterBias);
					EXPECT_NE(Cached->DepthBias, DynamicA->DepthBias);
					// Each view owns its Uniform until both command lists have been recorded.
					const auto FirstViewUniform = Input.ViewUniform;
					ViewParameters.WorldOriginHigh.x += 100;
					BindingDraw.RasterBias = {3.0f, 4.0f, 0.0f};
					Input.ViewUniform = Commands.CreateUniformBufferRange(&ViewParameters, sizeof(ViewParameters));
					FMeshDrawCommandBindingCache CacheB;

					FObservedCommandList DynamicContextB(FrameB, {EMeshPipelinePass::ShadowDepth}, DynamicBHandle);
					const auto DynamicContextBResult = PassProcessor.AddMeshBatch(ShadowBatch, Input, CacheB, DynamicContextB, Cached);
					ASSERT_TRUE(DynamicContextBResult) << DynamicContextBResult.Error; DynamicB = FrameB.GetCommand(DynamicBHandle);
					EXPECT_FALSE(FrameB.GetCommand(DynamicAHandle));
					EXPECT_EQ(DynamicContextB.Added, 1u); EXPECT_EQ(DynamicContextB.Finalized, 1u);
					EXPECT_EQ(DynamicB->VertexBindings, Cached->VertexBindings);
					EXPECT_NE(DynamicB->VertexPassBindings, DynamicA->VertexPassBindings);
					EXPECT_EQ(DynamicB->DepthBias, *BindingDraw.RasterBias);
					EXPECT_NE(DynamicA->DepthBias, DynamicB->DepthBias);
					const auto Original = DynamicB;
					auto ExpectRejected = [&](std::shared_ptr<const FMeshDrawCommand> Existing) {
						FMeshDrawCommandBindingCache RejectedCache;
						FObservedCommandList Context(FrameB, {EMeshPipelinePass::ShadowDepth}, DynamicBHandle);
						const auto ContextResult = PassProcessor.AddMeshBatch(ShadowBatch, Input, RejectedCache, Context, std::move(Existing));
						EXPECT_EQ(ContextResult.Status, EShaderBuildStatus::Error);
						EXPECT_EQ(Context.Finalized, 0u);
						EXPECT_EQ(FrameB.GetCommand(DynamicBHandle), Original);
						EXPECT_EQ(FrameB.Num({EMeshPipelinePass::ShadowDepth}), 1u);
						EXPECT_EQ(DynamicB, Original);
					};
					// A matching shader/factory/pass is insufficient if the native PSO was built for another state.
					auto WrongPipeline = std::make_shared<FMeshPassPipeline>(*Pipeline);
					WrongPipeline->Description.Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::LineList;
					Input.Pipeline = WrongPipeline;
					ExpectRejected({}); Input.Pipeline = Pipeline;
					// Request-specific HitProxy bindings cannot reuse a view-independent command.
					Input.HitProxy = PrimitiveUniform;
					ExpectRejected(Cached); Input.HitProxy = {};
					EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), Draws);
					// Submission consumes captured geometry, not the source view or description.
					auto Detached = *DynamicB;
					auto NonIndexedDescription = std::make_shared<FMeshDrawDescription>(*BindingDraw.Command);
					NonIndexedDescription->Geometry.bIndexed = false;
					NonIndexedDescription->Geometry.FirstElement = 0;
					NonIndexedDescription->Geometry.VertexOffset = 0;
					NonIndexedDescription->Geometry.ElementCount = 3;
					auto NonIndexedDraw = BindingDraw; NonIndexedDraw.Command = NonIndexedDescription;
					auto NonIndexed = Detached;
					ASSERT_TRUE(NonIndexed.SetDrawParametersAndFinalize(*Primitive, NonIndexedDraw));
					EXPECT_FALSE(NonIndexed.IndexBuffer);
					EXPECT_EQ(std::get<FRHIDrawArguments>(NonIndexed.DrawParameters).VertexCount, 3u);
					const auto DepthTarget = GDynamicRHI->RHICreateTexture(Commands,
						FRHITextureCreateDesc::Create2D("MeshCommandSubmission", 8, 8, EPixelFormat::D32)
						.SetFlags(ETextureCreateFlags::DepthStencilTargetable | ETextureCreateFlags::ShaderResource));
					ASSERT_TRUE(DepthTarget);
					FRHIRenderPassInfo Pass; Pass.RenderTargetLayout = Pipeline->Description.RenderTargets;
					Pass.DepthStencilRenderTarget = DepthTarget; Pass.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
					Commands.BeginRenderPass(Pass, "MeshCommandSubmission");
					Commands.SetViewport(0, 0, 0, 8, 8, 1);
					FMeshDrawCommandStateCache SubmitCache;
					EXPECT_FALSE(Cached->SubmitDraw(Commands, &SubmitCache));
					EXPECT_TRUE(Detached.SubmitDraw(Commands, &SubmitCache));
					EXPECT_TRUE(NonIndexed.SubmitDraw(Commands, &SubmitCache));
					EXPECT_TRUE(DynamicA->SubmitDraw(Commands, &SubmitCache));
					const auto BeforeRepeat = Commands.GetNumRecordedCommands();
					EXPECT_TRUE(DynamicA->SubmitDraw(Commands, &SubmitCache));
					EXPECT_EQ(Commands.GetNumRecordedCommands(), BeforeRepeat + 1);
					EXPECT_EQ(SubmitCache.Pipeline, DynamicA->Pipeline);
					Commands.EndRenderPass();
					EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), Draws + 4);
					EXPECT_EQ(PSOPrecacheValidation::GetStatistics().Hit, 1u); // Four draws, one full PSO identity.
					EXPECT_EQ(PSOPrecacheValidation::GetStatistics().Missed, 0u);

					BindingContextsValidated = true;
				}
			}
		});
		FlushRenderingCommands();
		if (!DirectReady || !CommandReady) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while ((!DirectReady || !CommandReady) && DirectError.empty() && std::chrono::steady_clock::now() < DirectDeadline);
	ASSERT_TRUE(DirectReady) << DirectError;
	ASSERT_TRUE(CommandReady);
	ASSERT_TRUE(BindingContextsValidated);
	ASSERT_EQ(DirectInitializers.size(), 1u + (InitialConfig.bSupportsWireframe ? 8u : 4u));
	const auto DirectEntries = std::span(DirectInitializers).subspan(1);
	EXPECT_EQ(std::ranges::count_if(DirectEntries, [](const auto& Entry) { return Entry.bRequired; }), 2);
	EXPECT_TRUE(std::ranges::any_of(DirectEntries, [](const auto& Entry) { return !Entry.bRequired; }));

	FTextureRHIRef ConfigTarget;
	struct FConfigTargetCleanup {
		FTextureRHIRef& Target;
		~FConfigTargetCleanup() {
			EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&Target = Target](FRHICommandListImmediate&) { Target = {}; });
			FlushRenderingCommands();
		}
	} TargetCleanup{ConfigTarget};
	FSceneView ConfigView;
	ConfigView.ViewportWidth = ConfigView.ViewportHeight = 64;
	const auto ViewPreparation = FRenderResourcePreparation::Create();
	const auto PrepareDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	do {
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			FGeometryQualificationFrame Frame(Commands);
			Renderer.BeginFrame_RenderThread(Commands);
			if (!ConfigTarget) ConfigTarget = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("PreparationConfigCoverage", 64, 64, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy));
			ASSERT_TRUE(ConfigTarget);
			Renderer.PrepareViewResources(Commands, {LowScene.get(), ConfigView, ConfigTarget}, ViewPreparation);
		});
		FlushRenderingCommands();
		if (ViewPreparation.GetProgress().State != ERenderResourcePreparationState::Pending) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < PrepareDeadline);
	ASSERT_EQ(ViewPreparation.GetProgress().State, ERenderResourcePreparationState::Ready);
	ASSERT_TRUE(Renderer.SetPreparationSettings(InitialConfig.Settings));
	FlushRenderingCommands();
	EXPECT_EQ(ViewPreparation.GetProgress().State, ERenderResourcePreparationState::Ready);
	std::shared_ptr<const FHitProxyMap> SceneLessPicking;
	FHitProxyRenderRequest SceneLessRequest;
	SceneLessRequest.View = ConfigView;
	SceneLessRequest.bSceneGeometry = false;
	const auto PickingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	do {
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			FGeometryQualificationFrame Frame(Commands);
			SceneLessPicking = Renderer.RenderHitProxies(Commands, nullptr, SceneLessRequest);
		});
		FlushRenderingCommands();
		if (!SceneLessPicking) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (!SceneLessPicking && std::chrono::steady_clock::now() < PickingDeadline);
	ASSERT_TRUE(SceneLessPicking);
	std::vector<FMaterialPSOPrecacheRequestID> UpdatedRequests;
	FScopedRenderThreadBlocker ConfigUpdateBlocker;
	struct FConfigUpdateDrain {
		FScopedRenderThreadBlocker& Blocker;
		~FConfigUpdateDrain() { Blocker.Release(); FlushRenderingCommands(); }
	} ConfigUpdateDrain{ConfigUpdateBlocker};
	// This frame is queued before the policy update but sees the future admission.
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
		FGeometryQualificationFrame Frame(Commands);
		Renderer.BeginFrame_RenderThread(Commands);
		ASSERT_EQ(UpdatedRequests.size(), 1u);
		EXPECT_EQ(UpdatedRequests[0].GetProgress().State, EMaterialPSOPrecacheState::Pending);
		EXPECT_FALSE(UpdatedRequests[0].GetProgress().bRequiredReady);
	});
	auto Settings = InitialConfig.Settings;
	Settings.bHybrid = false;
	ASSERT_TRUE(Renderer.SetPreparationSettings(Settings));
	Settings.bHitProxy = false;
	ASSERT_TRUE(Renderer.SetPreparationSettings(Settings));
	EXPECT_FALSE(Repeated[0].IsCurrent());
	EXPECT_FALSE(Repeated[0].GetProgress().bRequiredReady);
	const auto Updated = Renderer.GetSceneRenderPreparationConfig(LowScene->GetFeatureLevel());
	EXPECT_GT(Updated.Version, InitialConfig.Version);
	// Admit before the queued render-thread update: it must survive old-work retirement.
	ASSERT_TRUE(Material->PrecachePSOs(Renderer, LowScene->GetFeatureLevel(), Factories, {}, EPSOPrecachePriority::Medium, UpdatedRequests));
	ASSERT_EQ(UpdatedRequests.size(), 1u);
	EXPECT_NE(UpdatedRequests[0].GetValue(), Repeated[0].GetValue());
	ConfigUpdateBlocker.Release();
	FlushRenderingCommands();
	EXPECT_TRUE(UpdatedRequests[0].IsCurrent());
	EXPECT_EQ(ViewPreparation.GetProgress().State, ERenderResourcePreparationState::Stale);
	const auto UpdatedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!UpdatedRequests[0].GetProgress().bRequiredReady && std::chrono::steady_clock::now() < UpdatedDeadline) {
		EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
			FGeometryQualificationFrame Frame(Commands);
			Renderer.BeginFrame_RenderThread(Commands);
		});
		FlushRenderingCommands();
		DPrimitiveComponent::ProcessPSOPrecacheRequests(LowWorld.Get());
		DPrimitiveComponent::ProcessPSOPrecacheRequests(HighWorld.Get());
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(UpdatedRequests[0].GetProgress().bRequiredReady) << UpdatedRequests[0].GetDiagnostic();
	Settings.bForward = false;
	ASSERT_TRUE(Renderer.SetPreparationSettings(Settings));
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
		FGeometryQualificationFrame Frame(Commands);
		EXPECT_EQ(Renderer.RenderView(Commands, {LowScene.get(), ConfigView, ConfigTarget}), ERenderViewResult::RendererResourcesUnavailable);
		FHitProxyRenderRequest Picking;
		Picking.View = ConfigView;
		EXPECT_FALSE(Renderer.RenderHitProxies(Commands, LowScene.get(), Picking));
	});
	FlushRenderingCommands();
}
