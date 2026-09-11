#include "GeometrySubmissionTestSupport.h"
#include "LightSceneTestSupport.h"
#include "Renderers/SceneRendererProfiling.h"
#include "ShaderBuild/ShaderPaths.h"
#include "ShaderBuild/ShaderBuildLifecycle.h"
#include "Modules/ModuleTestSupport.h"
#include "Misc/FileHelper.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "SceneView.h"
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
#include "Rendering/MeshBatch.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
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
#include "SceneInfo.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshResources.h"

#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"

#include "NativeDObjectTestSupport.h"

#include <chrono>
#include <condition_variable>
#include <format>
#include <iostream>
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
		static Durin::FObjectHandle RootHandle;
		auto* Root = Durin::Cast<Durin::DMaterial>(Durin::ResolveObjectHandle(RootHandle));
		if (!Durin::IsValid(Root))
		{
			Root = Durin::NewObject<Durin::DMaterial>(nullptr, "PreparationVariantRoot");
			RootHandle = Durin::MakeObjectHandle(Root);
		}
		auto* Material = Durin::NewObject<Durin::DMaterialInstance>(nullptr, Name);
		Durin::FMaterialPropertyOverrides Overrides;
		Overrides.bOverrideBlendMode = true;
		Overrides.bOverrideOpacityMaskThreshold = true;
		Overrides.bOverrideTwoSided = true;
		Overrides.bOverrideDepthWritePolicy = true;
		Overrides.Values.BlendMode = BlendMode;
		Overrides.Values.bTwoSided = bTwoSided;
		Overrides.Values.DepthWritePolicy = DepthWrite;
		Overrides.Values.OpacityMaskThreshold = 0.4f;
		EXPECT_TRUE(Material->SetParentAndPropertyOverrides(Root, Overrides));
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
	ASSERT_TRUE(StaticMesh->TryReplaceRenderData(MakeRenderData(), {
		{.Name = Durin::FName("Section0"), .SourceMaterialIndex = 0},
		{.Name = Durin::FName("Section1"), .SourceMaterialIndex = 1},
		{.Name = Durin::FName("Section2"), .SourceMaterialIndex = 2},
		{.Name = Durin::FName("Section3"), .SourceMaterialIndex = 3}}, Error)) << Error;
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	auto CheckCpuOnlyAndGpuRetry = [](auto* Mesh) {
		const auto Before = Mesh->GetRenderResourceStatus();
		const auto Loaded = Mesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(Loaded.Succeeded()) << Loaded.Message;
		EXPECT_EQ(Loaded.Status.GpuPhase, Durin::ECookedMeshGpuPhase::Unavailable);
		EXPECT_FALSE(Mesh->HasPendingRenderResourceInitialization());
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Before.Revision);
		const auto Repeated = Mesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(Repeated.Succeeded()) << Repeated.Message;
		EXPECT_EQ(Repeated.Status.Generation, Loaded.Status.Generation);
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Before.Revision);
		{
			FScopedRenderThreadBlocker Blocker;
			Durin::VulkanRHI::ArmVulkanCreateFailure(
				Durin::VulkanRHI::EVulkanCreateFailurePoint::Buffer);
			Mesh->InitResources();
			const auto Queued = Mesh->GetRenderResourceStatus();
			const auto WhileQueued = Mesh->EnsureRenderDataLoadedBlocking();
			EXPECT_TRUE(WhileQueued.Succeeded());
			EXPECT_EQ(WhileQueued.Status.GpuPhase, Durin::ECookedMeshGpuPhase::Queued);
			EXPECT_FALSE(Mesh->GetRenderResourceStatus().IsReady());
			EXPECT_TRUE(Mesh->HasPendingRenderResourceInitialization());
			Mesh->InitResources();
			EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Queued.Revision);
		}
		Durin::FlushRenderingCommands();
		const auto Failed = Mesh->RequestRenderDataAndResources();
		EXPECT_EQ(Failed.GpuPhase, Durin::ECookedMeshGpuPhase::Failed);
		EXPECT_FALSE(Mesh->HasPendingRenderResourceInitialization());
		EXPECT_EQ(Mesh->GetRenderData()->GetNumInitializedResources(), 0u);
		for (int Index = 0; Index < 3; ++Index)
		{
			const auto Polled = Mesh->RequestRenderDataAndResources();
			EXPECT_EQ(Polled.GpuPhase, Durin::ECookedMeshGpuPhase::Failed);
			EXPECT_EQ(Polled.ResourceRevision, Failed.ResourceRevision);
			const auto CpuOnly = Mesh->EnsureRenderDataLoadedBlocking();
			EXPECT_TRUE(CpuOnly.Succeeded());
			EXPECT_EQ(CpuOnly.Status.GpuPhase, Durin::ECookedMeshGpuPhase::Failed);
			EXPECT_EQ(CpuOnly.Status.ResourceRevision, Failed.ResourceRevision);
		}
		Mesh->InitResources();
		Durin::FlushRenderingCommands();
		const auto Ready = Mesh->GetRenderResourceStatus();
		EXPECT_TRUE(Ready.IsReady());
		EXPECT_FALSE(Mesh->HasPendingRenderResourceInitialization());
		EXPECT_GT(Ready.Revision, Failed.ResourceRevision);
		Mesh->InitResources();
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Ready.Revision);
	};
	CheckCpuOnlyAndGpuRetry(StaticMesh);
	Durin::MarkAsGarbage(StaticMesh);
	Durin::CollectGarbage();
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
	ASSERT_TRUE(Durin::InitializeAssetManager());
	Durin::FPackagePath AuthoredPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/CookedStaticMeshRenderPreparation/Mesh", AuthoredPath));
	Durin::DStaticMesh* AuthoredMesh = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(AuthoredPath, AuthoredMesh));
	ASSERT_NE(AuthoredMesh, nullptr);
	std::string Error;
	ASSERT_TRUE(AuthoredMesh->TryReplaceRenderData(MakeRenderData(), {
			{.Name = Durin::FName("Section0"), .SourceMaterialIndex = 0},
			{.Name = Durin::FName("Section1"), .SourceMaterialIndex = 1},
			{.Name = Durin::FName("Section2"), .SourceMaterialIndex = 2},
			{.Name = Durin::FName("Section3"), .SourceMaterialIndex = 3}}, Error)) << Error;

	Durin::FCookContext CookContext(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*AuthoredMesh, "/Game/CookedMesh", CookContext, Error)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(CookContext, CookRoot, &Error)) << Error;
	ASSERT_TRUE(Durin::UnloadPackage(
		AuthoredPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();

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
	for (bool bUseManager : {true, false})
	{
		if (!bUseManager) Durin::ShutdownCookedMeshLoadManager();
		const auto CpuOnly = CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(CpuOnly.Succeeded()) << CpuOnly.Message;
		EXPECT_EQ(CpuOnly.Status.GpuPhase, Durin::ECookedMeshGpuPhase::Unavailable);
		EXPECT_EQ(CookedMesh->GetRenderData()->GetNumInitializedResources(), 0u);
		ASSERT_TRUE(Durin::UnloadPackage(CookedPath));
		CookedMesh = nullptr;
		ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
			CookedPath, AuthoredPath.GetPackageName()), CookedMesh));
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
	const Durin::FPrimitiveComponentId PrimitiveId(211);
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
					CommandList, Scene.GetPrimitiveSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			ASSERT_EQ(Prepared.Primitives.size(), 1u);
			// MakeRenderData supplies four sections on the single cooked primitive.
			EXPECT_EQ(Prepared.GetNumSections(), 4u);
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
	Durin::InitializeAssetManager();
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
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate&) {
			using namespace Durin;
			FMeshCollectionContext Context;
			Context.PrimitiveId = FPrimitiveComponentId(700);
			Context.WorldBounds = RenderData->LocalBounds;
			Context.ProjectionStatus = EMeshCollectionProjectionStatus::Valid;
			FStaticMeshSceneProxy StaticProxy(RenderData.get(), {Opaque, Masked, Translucent, Opaque}, 1);
			FMeshBatchCollector Before(EMeshCollectionPurpose::Receiver);
			StaticProxy.CollectMeshBatches(Context, Before);
			ASSERT_EQ(Before.GetBatches().size(), 1u);
			const auto& Original = Before.GetBatches().front();
			ASSERT_EQ(Original.Elements.size(), 4u);
			EXPECT_EQ(Original.Elements[1].Draw.FirstElement, 3u);
			EXPECT_EQ(Original.Elements[1].Material.PlanningPassIdentity.ShaderMap.BlendMode, EMaterialBlendMode::Masked);
			StaticProxy.UpdateMaterialBinding_RenderThread({0, Translucent, 2});
			FMeshBatchCollector After(EMeshCollectionPurpose::Receiver);
			StaticProxy.CollectMeshBatches(Context, After);
			ASSERT_EQ(After.GetBatches().size(), 1u);
			EXPECT_EQ(After.GetBatches()[0].Elements[0].Material.PlanningPassIdentity.ShaderMap.BlendMode, EMaterialBlendMode::Translucent);
			EXPECT_EQ(Original.Elements[0].Material.PlanningPassIdentity.ShaderMap.BlendMode, EMaterialBlendMode::Opaque);
			FSplineMeshRenderDynamicData Dynamic{.LocalBounds = Context.WorldBounds, .Revision = 1};
			FSplineMeshSceneProxy SplineProxy(RenderData.get(), {Opaque, Masked, Translucent, Opaque}, 1, Dynamic);
			FMeshBatchCollector SplineBefore(EMeshCollectionPurpose::Receiver);
			SplineProxy.CollectMeshBatches(Context, SplineBefore);
			ASSERT_EQ(SplineBefore.GetBatches().size(), 1u);
			const auto OldBinding = std::dynamic_pointer_cast<const FSplineMeshBatchBinding>(SplineBefore.GetBatches()[0].Binding);
			ASSERT_TRUE(OldBinding);
			Dynamic.Revision = 2;
			Dynamic.Params.EndPosition = {12.0, 34.0, 56.0};
			ASSERT_TRUE(SplineProxy.UpdateDynamicData_RenderThread(Dynamic));
			FMeshBatchCollector SplineAfter(EMeshCollectionPurpose::Shadow);
			Context.Purpose = EMeshCollectionPurpose::Shadow;
			SplineProxy.CollectMeshBatches(Context, SplineAfter);
			ASSERT_EQ(SplineAfter.GetBatches().size(), 1u);
			const auto NewBinding = std::dynamic_pointer_cast<const FSplineMeshBatchBinding>(SplineAfter.GetBatches()[0].Binding);
			ASSERT_TRUE(NewBinding);
			EXPECT_EQ(OldBinding->DynamicData.Revision, 1u);
			EXPECT_EQ(NewBinding->DynamicData.Revision, 2u);
			EXPECT_EQ(NewBinding->DynamicData.Params.EndPosition, Dynamic.Params.EndPosition);
			EXPECT_NE(OldBinding.get(), NewBinding.get());
			Context.WorldBounds = FBox{};
			FMeshBatchCollector Invalid(EMeshCollectionPurpose::Receiver);
			StaticProxy.CollectMeshBatches(Context, Invalid);
			EXPECT_TRUE(Invalid.GetBatches().empty());
			EXPECT_EQ(Invalid.GetOutcomeCount(EGeometrySubmissionOutcome::InvalidSubmission), 1u);
			FStaticMeshSceneProxy Unavailable(nullptr, {}, 1);
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
		.LocalBounds = Durin::FBox({-2.0, -2.0, -1.0}, {120.0, 40.0, 20.0}),
		.Revision = 1};
	SplineDynamic.Params.EndPosition = {100.0, 30.0, 10.0};
	SplineDynamic.Params.EndTangent = {80.0, 0.0, 10.0};
	SplineDynamic.Params.SourceForwardMin = -1.0;
	SplineDynamic.Params.SourceForwardMax = 1.0;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(SplineScene, Durin::FPrimitiveComponentId(72),
		std::make_unique<Durin::FSplineMeshSceneProxy>(RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent, Opaque},
			1, SplineDynamic), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&SplineScene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Prepared =
				Durin::PrepareStaticMeshView_RenderThread(CommandList, SplineScene.GetPrimitiveSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			ASSERT_EQ(Prepared.Primitives.size(), 1u);
			EXPECT_EQ(Prepared.Primitives[0].VertexDomain,
				Durin::EVertexDeformationDomain::Spline);
			EXPECT_EQ(std::dynamic_pointer_cast<const Durin::FSplineMeshBatchBinding>(Prepared.Primitives[0].CollectedBinding)->DynamicData.Revision, 1u);
			ASSERT_FALSE(Prepared.Opaque.empty());
			EXPECT_EQ(Prepared.Opaque[0].PipelineKey.VertexDomain,
				Durin::EVertexDeformationDomain::Spline);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::ReleaseScene(SplineSceneOwner);
	Durin::FlushRenderingCommands();
	const Durin::FPrimitiveComponentId Id(71);
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(RenderData.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 1), Durin::Math::ScaleMatrix(Durin::FVector3(-1.0, 1.0, 1.0)));
	Durin::FlushRenderingCommands();

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene, Summary](Durin::FRHICommandListImmediate& CommandList) {
			Durin::FSceneView FirstView;
			FirstView.Settings.Mode.TranslucentSortPolicy = Durin::ETranslucentSortPolicy::Distance;
			FirstView.ViewLocation = Durin::FVector3(0.0, 0.0, -10.0);
			const Durin::FPreparedStaticMeshView First =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(), FirstView,
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
			EXPECT_EQ(OpaqueSections, (std::array<uint64, 2>{0u, 3u}));
			EXPECT_EQ(First.Masked[0].SectionIndex, 1u);
			EXPECT_EQ(First.Translucent[0].SectionIndex, 2u);
			EXPECT_EQ(
				First.Translucent[0].Material.PlanningPassIdentity.ShaderMap,
				First.Translucent[0].PipelineKey.Material.ShaderMap
			);
			Summary->MirroredFrontFace =
				First.Opaque.front().PipelineKey.Rasterizer.FrontFace;
			Summary->bTranslucentDepthWrite =
				First.Translucent.front().PipelineKey.Depth.bEnableWrite;
			Summary->bTranslucentBlend =
				First.Translucent.front().PipelineKey.ColorBlend.bEnable;
			Summary->FirstViewDistance =
				First.Translucent.front().TranslucentSortDepth;

			Durin::FSceneView SecondView;
			SecondView.Settings.Mode.TranslucentSortPolicy = Durin::ETranslucentSortPolicy::Distance;
			SecondView.ViewLocation = Durin::FVector3(0.0, 0.0, 5.0);
			const Durin::FPreparedStaticMeshView Second =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(), SecondView,
					Durin::ERasterMode::Solid
				);
			ASSERT_EQ(Second.GetNumSections(), First.GetNumSections());
			Summary->SecondViewDistance =
				Second.Translucent.front().TranslucentSortDepth;
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
		Durin::FPrimitiveComponentId(90),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>(4, Translucent),
			1),
		Durin::Math::TranslationMatrix(Durin::FVector3(0.0, 0.0, 20.0)));
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(OrderingScene,
		Durin::FPrimitiveComponentId(80),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			RenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>(4, Translucent),
			1),
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
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(OrderingScene, Durin::FPrimitiveComponentId(80));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(OrderingScene, Durin::FPrimitiveComponentId(90));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner GroupingSceneOwner;

	Durin::FScene& GroupingScene = *GroupingSceneOwner;
	auto AddGroupingPrimitive = [&](uint64 PrimitiveId) {
		Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(GroupingScene,
			Durin::FPrimitiveComponentId(PrimitiveId),
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
					Draw.SectionIndex);
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
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveComponentId(100));
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
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveComponentId(100));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(GroupingScene, Durin::FPrimitiveComponentId(200));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner MultiLODSceneOwner;

	Durin::FScene& MultiLODScene = *MultiLODSceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(MultiLODScene,
		Durin::FPrimitiveComponentId(101),
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
				Middle.Opaque[0].Indices.Buffer,
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
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(MultiLODScene, Durin::FPrimitiveComponentId(101));
	Durin::FlushRenderingCommands();

	Durin::FSceneTestOwner DegenerateTransformSceneOwner;

	Durin::FScene& DegenerateTransformScene = *DegenerateTransformSceneOwner;
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(DegenerateTransformScene,
		Durin::FPrimitiveComponentId(102),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			MultiLODRenderData.get(),
			std::vector<Durin::FMaterialRenderProxyRef>{Opaque}, 2),
		Durin::Math::ScaleMatrix(Durin::FVector3(0.0, 1.0, 1.0)));
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(DegenerateTransformScene,
		Durin::FPrimitiveComponentId(103),
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
					DegenerateTransformScene.GetPrimitiveSceneInfos(),
					Durin::FSceneView{}, Durin::ERasterMode::Solid);
			EXPECT_TRUE(Prepared.Primitives.empty());
			EXPECT_EQ(Prepared.GetNumSections(), 0u);
			EXPECT_EQ(Prepared.RejectedPrimitives, 2u);
		});
	Durin::FlushRenderingCommands();
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(DegenerateTransformScene, Durin::FPrimitiveComponentId(102));
	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(DegenerateTransformScene, Durin::FPrimitiveComponentId(103));
	Durin::FlushRenderingCommands();

	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(RenderData.get(), std::vector<Durin::FMaterialRenderProxyRef>{Opaque, Masked, Translucent}, 2), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&Scene](Durin::FRHICommandListImmediate& CommandList) {
			const Durin::FPreparedStaticMeshView Replaced =
				Durin::PrepareStaticMeshView_RenderThread(
					CommandList, Scene.GetPrimitiveSceneInfos(), Durin::FSceneView{},
					Durin::ERasterMode::Solid
				);
			ASSERT_EQ(Replaced.GetNumSections(), 4u);
			EXPECT_EQ(Replaced.Opaque.front().PipelineKey.Rasterizer.FrontFace, Durin::ERHIFrontFace::Clockwise);
		}
	);
	Durin::FlushRenderingCommands();

	Durin::FSceneInterfaceTestAccess::TryRemovePrimitiveProxy(Scene, Id);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());
	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			EXPECT_EQ(Durin::PrepareStaticMeshView_RenderThread(
				CommandList, Scene.GetPrimitiveSceneInfos(), Durin::FSceneView{},
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

TEST(FStaticMeshRenderPreparationVulkanTests,
	RecordsMixedGeometryPreparationBaseline)
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

	constexpr size_t PrimitiveCount = 64;
	for (size_t Index = 0; Index < PrimitiveCount; ++Index)
	{
		std::unique_ptr<Durin::FPrimitiveSceneProxy> Proxy;
		if (Index % 2 == 0)
		{
			Proxy = std::make_unique<Durin::FStaticMeshSceneProxy>(
				RenderData.get(), Materials, 1);
		}
		else
		{
			Durin::FSplineMeshRenderDynamicData Dynamic{
				.Params = {},
				.LocalBounds = Durin::FBox({-2.0, -2.0, -1.0}, {120.0, 40.0, 20.0}),
				.Revision = 1};
			Dynamic.Params.EndPosition = {100.0, 30.0, 10.0};
			Dynamic.Params.EndTangent = {80.0, 0.0, 10.0};
			Dynamic.Params.SourceForwardMin = -1.0;
			Dynamic.Params.SourceForwardMax = 1.0;
			Proxy = std::make_unique<Durin::FSplineMeshSceneProxy>(
				RenderData.get(), Materials, 1, Dynamic);
		}
		EXPECT_TRUE(Durin::FSceneInterfaceTestAccess::TryAddPrimitiveProxy(
			Scene, Durin::FPrimitiveComponentId(1000 + Index), std::move(Proxy),
			Durin::FMatrix(1.0)));
	}
	Durin::FlushRenderingCommands();

	Durin::EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>(
		[&](Durin::FRHICommandListImmediate& CommandList) {
			constexpr size_t WarmupFrames = 30;
			constexpr size_t MeasuredFrames = 120;
			// This is owned top-level container capacity, not allocator or
			// transitive material/RHI memory accounting.
			auto ContainerBytes = [](const Durin::FPreparedStaticMeshView& Prepared) {
				return Prepared.Primitives.capacity() * sizeof(Durin::FPreparedStaticMeshPrimitive)
					+ (Prepared.Opaque.capacity() + Prepared.Masked.capacity()
						+ Prepared.Translucent.capacity()) * sizeof(Durin::FPreparedStaticMeshDraw)
					+ (Prepared.RequestedLODHistogram.capacity()
						+ Prepared.SelectedLODHistogram.capacity()) * sizeof(size_t);
			};
			RecordProperty("baseline_primitives", std::to_string(PrimitiveCount));
			RecordProperty("baseline_draws", "256");
			RecordProperty("baseline_warmup_frames", std::to_string(WarmupFrames));
			RecordProperty("baseline_measured_frames", std::to_string(MeasuredFrames));
			RecordProperty("baseline_timing_authority", "diagnostic");
			for (size_t Run = 0; Run < 3; ++Run)
			{
				std::array<double, MeasuredFrames> Nanoseconds{};
				size_t PeakContainerBytes = 0;
				for (size_t Frame = 0; Frame < WarmupFrames + MeasuredFrames; ++Frame)
				{
					const auto Start = std::chrono::steady_clock::now();
					const auto Prepared = Durin::PrepareStaticMeshView_RenderThread(
						CommandList, Scene.GetPrimitiveSceneInfos(), Durin::FSceneView{},
						Durin::ERasterMode::Solid);
					const auto End = std::chrono::steady_clock::now();
					ASSERT_EQ(Prepared.Primitives.size(), PrimitiveCount);
					ASSERT_EQ(Prepared.Opaque.size(), 128u);
					ASSERT_EQ(Prepared.Masked.size(), 64u);
					ASSERT_EQ(Prepared.Translucent.size(), 64u);
					ASSERT_EQ(Prepared.RejectedPrimitives, 0u);
					ASSERT_EQ(Prepared.PreparedSplinePrimitives, 32u);
					if (Frame >= WarmupFrames)
					{
						Nanoseconds[Frame - WarmupFrames] =
							std::chrono::duration<double, std::nano>(End - Start).count();
						PeakContainerBytes = std::max(PeakContainerBytes, ContainerBytes(Prepared));
					}
				}
				std::ranges::sort(Nanoseconds);
				const double Median = (Nanoseconds[59] + Nanoseconds[60]) / 2.0;
				const double P95 = Nanoseconds[113]; // Nearest rank: ceil(0.95 * 120).
				std::cout << std::format("[GeometryPreparationBaseline] run={} median_ns={} p95_ns={} peak_container_bytes={} authority=diagnostic\n",
					Run + 1, Median, P95, PeakContainerBytes);
				RecordProperty(std::format("baseline_run_{}_median_ns", Run + 1), std::to_string(Median));
				RecordProperty(std::format("baseline_run_{}_p95_ns", Run + 1), std::to_string(P95));
				RecordProperty(std::format("baseline_run_{}_peak_container_bytes", Run + 1), std::to_string(PeakContainerBytes));
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
	// Own this target's shader lifecycle so extension mounts precede the freeze.
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
			InitializeShaderBuildForTesting();
		}
		auto TearDown() -> void override { ShutdownShaderBuild(); }
	};
	[[maybe_unused]] const auto GeometryEnvironment = testing::AddGlobalTestEnvironment(new FGeometryQualificationEnvironment());
}

TEST(FStaticMeshRenderPreparationVulkanTests, QualifiesIndependentMultiBatchGeometryAndFactory)
{
	using namespace Durin;
	using namespace Durin::Tests;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId();
	GIsGameThreadIdInitialized = true;
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(InitializeAssetManager());
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModule("RenderCore");

	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness RendererLifecycle("GeometryQualification");
	RendererLifecycle.Start(Renderer);
	FSceneTestOwner Owner;
	auto& Scene = *Owner;
	const auto Opaque = MakeMaterial(EMaterialBlendMode::Opaque, true);
	const auto Masked = MakeMaterial(EMaterialBlendMode::Masked, true);
	std::array<FProceduralGeometry, 2> Geometries;
	std::array<FMaterialRenderData, 2> Materials;
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& CommandList) {
		Materials = {Opaque->Resolve_RenderThread(), Masked->Resolve_RenderThread()};
		Geometries[0] = FProceduralGeometry::Create(CommandList, 1, false);
		Geometries[1] = FProceduralGeometry::Create(CommandList, 2, true);
		Geometries[1].Binding->DisplacementScale = {0.05f, 0.0f, 0.0f, 0.8f};
	});
	FlushRenderingCommands();
	for (size_t I = 0; I < Geometries.size(); ++I)
	{
		auto Proxy = std::make_unique<FProceduralProxy>();
		Proxy->Geometry = Geometries[I]; Proxy->Materials = Materials; Proxy->bReverse = I == 1;
		ASSERT_TRUE(FSceneInterfaceTestAccess::TryAddPrimitiveProxy(Scene, FPrimitiveComponentId(100 + I), std::move(Proxy), FMatrix(1.0)));
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
		EXPECT_EQ(Snapshot.VisibleCandidates, 2u);
		EXPECT_EQ(Snapshot.PreparedLocalPrimitives, 2u);
		EXPECT_EQ(Snapshot.RejectedPrimitives, 0u);
		ASSERT_EQ(Snapshot.GetNumSections(), 4u);
		EXPECT_EQ(Snapshot.Opaque.size(), 2u);
		EXPECT_EQ(Snapshot.Masked.size(), 2u);
		EXPECT_GE(Snapshot.Opaque.front().SectionIndex, uint64{1} << 48);
		EXPECT_GE(Snapshot.Primitives.front().BatchId, uint64{1} << 40);
		EXPECT_EQ(Snapshot.Masked.front().Geometry.FirstElement, 3u);
		EXPECT_EQ(Snapshot.Masked.front().Geometry.FirstInstance, 2u);
		EXPECT_EQ(Snapshot.Masked.front().Geometry.InstanceCount, 2u);
		const auto Shadow = PrepareStaticMeshView_RenderThread(CommandList, Visible.SceneInfos, View, ERasterMode::Solid, ERenderPreparationMode::ShadowDepth);
		EXPECT_EQ(Shadow.GetNumSections(), 2u);
		EXPECT_EQ(Shadow.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Excluded)], 2u);
		auto Invalid = *Geometries[0].Binding;
		Invalid.Streams[0].Offset = Invalid.Streams[0].VertexBuffer->GetSize();
		EXPECT_EQ(Invalid.ValidateInputs(Snapshot.Opaque.front().Geometry), EGeometrySubmissionOutcome::InvalidSubmission);
		Invalid = *Geometries[0].Binding;
		Invalid.DeclarationElements[0].Stride = 1;
		EXPECT_EQ(Invalid.ValidateInputs(Snapshot.Opaque.front().Geometry), EGeometrySubmissionOutcome::InvalidSubmission);
		const auto MissingProxy = std::make_shared<FProceduralProxy>();
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
		const auto Desc = FRHITextureCreateDesc::Create2D("IndependentGeometryColor", 192, 108, EPixelFormat::SRGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
		const auto Target = GDynamicRHI->RHICreateTexture(CommandList, Desc);
		ASSERT_NE(Target, nullptr);
		for (auto Mode : {ERenderMode::Unlit, ERenderMode::Lit})
		{
			View.Settings.Mode.RenderMode = Mode;
			++GRenderFrameCounterRenderThread;
			GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			EXPECT_EQ(Renderer.RenderView(CommandList, &Scene, View, Target, false, {}), ERenderViewResult::Success);
			GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		}
	});
	FlushRenderingCommands();
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
		ASSERT_TRUE(FSceneInterfaceTestAccess::TryAddPrimitiveProxy(PairScene, FPrimitiveComponentId(200), std::move(Proxy), Transform));
		FDirectionalLightSceneData Light;
		Light.Intensity = 3.0f;
		ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(PairScene, FLightComponentId(1), Light), nullptr);
	}
	FlushRenderingCommands();
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& CommandList) {
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
				const auto Target = GDynamicRHI->RHICreateTexture(CommandList, Desc);
				++GRenderFrameCounterRenderThread;
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
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
				const auto Result = Renderer.RenderView(CommandList, I == 0 ? &*ReferenceOwner : &*DeformedOwner, View, Target, false, {});
				SetGBufferCaptureSink(nullptr);
				SetShadowDepthCaptureSink(nullptr);
				ShadowCapture = nullptr;
				Capture = nullptr;
				ASSERT_EQ(Result, ERenderViewResult::Success);
				ReadGeometryTexture(CommandList, Target, Pixels[I]);
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
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
	});
	FlushRenderingCommands();
	ReferenceOwner.Reset(); DeformedOwner.Reset();
	// Optional GBuffer exclusion must preserve retained-forward and translucency.
	FSceneTestOwner ForwardOwner;
	FProceduralGeometry ForwardGeometry;
	const auto Translucent = MakeMaterial(EMaterialBlendMode::Translucent, true);
	FMaterialRenderData TranslucentData;
	EnqueueRenderCommand<FCapturePreparedStaticMeshViewCommand>([&](FRHICommandListImmediate& Commands) {
		ForwardGeometry = FProceduralGeometry::Create(Commands, 500, true);
		ForwardGeometry.Binding->bForwardOnly = true;
		TranslucentData = Translucent->Resolve_RenderThread();
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
			EXPECT_EQ(Prepared.Opaque[0].bSupportsGBuffer, LocalFactory);
			const auto Shadow = PrepareStaticMeshView_RenderThread(Commands, ForwardOwner->GetPrimitiveSceneInfos(), View, ERasterMode::Solid, ERenderPreparationMode::ShadowDepth);
			EXPECT_EQ(Shadow.bResourceFailure, FailShadow);
			static FViewRenderTelemetry Telemetry;
			Telemetry = {};
			SetViewRenderTelemetrySink(+[](const FViewRenderTelemetry& Value) { Telemetry = Value; });
			const auto Target = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("ForwardOnlyGeometry", 192, 108, EPixelFormat::SRGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource));
			++GRenderFrameCounterRenderThread;
			GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
			const auto Result = Renderer.RenderView(Commands, &*ForwardOwner, View, Target, false, {});
			SetViewRenderTelemetrySink(nullptr);
			GDynamicRHI->RHIEndFrame_RenderThread(Commands);
			EXPECT_EQ(Result, ERenderViewResult::Success);
			EXPECT_EQ(Telemetry.GBuffer.GBufferSuccessfulDraws, LocalFactory ? 1u : 0u);
			EXPECT_EQ(Telemetry.CombinedTranslucentGeometryDraws, 1u);
			EXPECT_EQ(Telemetry.DirectionalShadow.ShadowPreparationFailures, FailShadow ? 1u : 0u);
			EXPECT_EQ(Telemetry.DirectionalShadow.ShadowSuccessfulDraws, FailShadow ? 0u : 1u);
		});
		FlushRenderingCommands();
	}
	ForwardOwner.Reset();
	FlushRenderingCommands();
	ForwardGeometry = {};
	// Replacement and detach cannot invalidate retained frame bindings/resources.
	auto OldBuffer = Snapshot.Opaque.front().Vertices.Buffer;
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
	EXPECT_EQ(Snapshot.Opaque.front().Vertices.Buffer, OldBuffer);
	EXPECT_NE(Geometries[0].Vertices.Buffer, OldBuffer);
	Owner.Reset();
	FlushRenderingCommands();
	OldBuffer = {};
	Snapshot = {};
	Geometries = {};
	RendererLifecycle.Shutdown();
	ShutdownRenderingThread();
	RHIExit();
}
