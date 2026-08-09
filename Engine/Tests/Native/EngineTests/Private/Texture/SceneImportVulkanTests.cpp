#include "AssetSystem.h"
#include "Asset/EditorAssetRetention.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/FPrimitiveSceneProxy.h"
#include "EngineTestSupport.h"
#include "Hash/XxHash.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/SceneVisibility.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMeshTestAccess.h"
#include "SceneImport.h"
#include "Thumbnail/AssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"
#include "Texture/Texture2D.h"
#include "TextureTestSupport.h"
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>

namespace
{
	std::vector<Durin::FViewRenderCounters>* GSceneImportCounterSnapshots = nullptr;

	auto CaptureSceneImportCounterSnapshot(
		const Durin::FViewRenderCounters& Counters) -> void
	{
		if (GSceneImportCounterSnapshots != nullptr)
		{
			GSceneImportCounterSnapshots->push_back(Counters);
		}
	}

	class FScopedSceneImportCounterSink final
	{
	public:
		explicit FScopedSceneImportCounterSink(
			std::vector<Durin::FViewRenderCounters>& Snapshots)
		{
			GSceneImportCounterSnapshots = &Snapshots;
			Durin::SetViewRenderCounterSink(CaptureSceneImportCounterSnapshot);
		}

		~FScopedSceneImportCounterSink()
		{
			Durin::SetViewRenderCounterSink(nullptr);
			GSceneImportCounterSnapshots = nullptr;
		}
	};

	class FSceneImportRenderEngine final : public Durin::DEngine
	{
	public:
		FSceneImportRenderEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}

		auto SetRenderer(Durin::IRendererModule* InRenderer) -> void
		{
			RendererModule = InRenderer;
		}
	};

	auto MakeAssetPath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Result;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Result));
		return Result;
	}
}

TEST(FSceneImportVulkanTests, RendersReloadedSrgbTextureAndBaseColorFactor)
{
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	Durin::FAssetPath SpherePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::FRenderedAssetThumbnailVisualContract::SphereVirtualPath, SpherePath));
	Durin::FRetainedEditorAsset PreloadedSphere;
	std::string Error;
	ASSERT_TRUE(Durin::FEditorAssetRetentionService::Acquire(
		SpherePath, PreloadedSphere, Error)) << Error;

	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "SceneImportVulkan";
	const std::filesystem::path BuiltInEnvironmentRoot =
		std::filesystem::path(Durin::FPaths::EngineContentDir()) / "Renderer";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FScopedDerivedDataCacheRoot DerivedDataCache(Root / "DerivedDataCache");
	for (const std::filesystem::path& Directory : {
		Root / "Engine/Content",
		Root / "Project/Content"})
	{
		std::filesystem::create_directories(Directory);
	}
	std::filesystem::create_directories(Root / "Engine/Content/Renderer");
	for (const std::string_view File : {
		"DefaultStudioEnvironment.dasset",
		"DefaultStudioEnvironment.iblbulk"})
	{
		std::filesystem::copy_file(
			BuiltInEnvironmentRoot / File,
			Root / "Engine/Content/Renderer" / File,
			std::filesystem::copy_options::overwrite_existing);
	}
	const std::array Mounts{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = Root / "Engine/Content",
			.bAutoScan = true,
			.bAuthoringWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/SceneImportVulkan/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = Root / "Project/Content",
			.bAutoScan = true,
			.bAuthoringWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ASSERT_TRUE(MountFixture.IsValid()) << MountFixture.GetError();
	std::string MaterialError;
	ASSERT_NE(Durin::EnsureStandardImportedSurfaceMaterial(MaterialError), nullptr)
		<< MaterialError;

	const Durin::FAssetPath DestinationDirectory =
		MakeAssetPath("/SceneImportVulkan/Imports/RenderedOpaque");
	const std::filesystem::path MountedScene =
		Root / "Project/Content/Models/RenderedOpaqueDataUri.gltf";
	std::filesystem::create_directories(MountedScene.parent_path());
	std::filesystem::copy_file(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/RenderedOpaqueDataUri.gltf",
		MountedScene,
		std::filesystem::copy_options::overwrite_existing);
	const Durin::FSceneImportPlanResult Planned = Durin::PlanSceneImport({
		.RootSource = {.Path = "/SceneImportVulkan/Models/RenderedOpaqueDataUri.gltf"},
		.DestinationDirectory = DestinationDirectory,
		.MeshSettings = Durin::FStaticMeshImportSettings::MakeDurin()});
	ASSERT_TRUE(Planned) << Planned.Message;
	ASSERT_EQ(Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs().size(), 3u);
	Durin::FAssetPath MeshPath;
	Durin::FAssetPath TexturePath;
	Durin::FAssetPath MaterialPath;
	for (const Durin::AssetImport::FImportOutputPreview& Output
		: Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs())
	{
		if (Output.AssetClassName
			== Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString())
			MeshPath = Output.AssetPath;
		else if (Output.AssetClassName
			== Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString())
			TexturePath = Output.AssetPath;
		else if (Output.AssetClassName
			== Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString())
			MaterialPath = Output.AssetPath;
	}
	ASSERT_TRUE(MeshPath.IsValid());
	ASSERT_TRUE(TexturePath.IsValid());
	ASSERT_TRUE(MaterialPath.IsValid());
	const Durin::FAssetPath StandardPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	const Durin::FSceneImportExecutionResult Executed =
		Durin::ExecuteSceneImport(Planned.Plan);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_EQ(Executed.Meshes.size(), 1u);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));

	const Durin::FAssetPath LODContractPath =
		MakeAssetPath("/SceneImportVulkan/LODContract");
	const Durin::FStaticMeshImportResult LODContractImport =
		Durin::DStaticMesh::ImportAsset(
			(std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "MultiSection.gltf").generic_string(),
			LODContractPath.ToString());
	ASSERT_TRUE(LODContractImport) << LODContractImport.Message;
	Durin::DStaticMesh* LODContractMesh = LODContractImport.Asset;
	ASSERT_NE(LODContractMesh, nullptr);
	const Durin::FStaticMeshRenderData* LODContractRenderData =
		LODContractMesh->GetRenderData();
	ASSERT_NE(LODContractRenderData, nullptr);
	ASSERT_EQ(LODContractRenderData->LODResources.size(), 1u);
	const Durin::FStaticMeshLODResources& LODContract =
		LODContractRenderData->LODResources[0];
	ASSERT_GT(LODContract.GetNumVertices(), 0u);
	const Durin::FStaticMeshRenderResourceStatus LODContractStatus =
		LODContractMesh->GetRenderResourceStatus();
	EXPECT_EQ(
		LODContractStatus.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Unavailable);
	EXPECT_NE(LODContractStatus.Revision, 0u);
	const std::optional<Durin::FBox> LOD0Bounds =
		LODContractMesh->GetLOD0LocalBounds();
	ASSERT_TRUE(LOD0Bounds.has_value());
	EXPECT_EQ(LOD0Bounds->Min, LODContract.LocalBounds.Min);
	EXPECT_EQ(LOD0Bounds->Max, LODContract.LocalBounds.Max);
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.VertexBuffers.StaticMeshVertexBuffer
			.TangentsVertexBuffer.GetNormals(),
		[](const Durin::FVector3f& Normal) {
			return std::abs(Normal.y) > 0.1f
				&& std::abs(Normal.z) < 0.99f;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.VertexBuffers.StaticMeshVertexBuffer
			.TangentsVertexBuffer.GetTangents(),
		[](const Durin::FVector4f& Tangent) {
			return Tangent.w < 0.0f;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.VertexBuffers.StaticMeshVertexBuffer
			.TexCoordVertexBuffer.GetTexCoords()[0],
		[](const Durin::FVector2f& UV) {
			return UV.x != 0.0f || UV.y != 0.0f;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.VertexBuffers.ColorVertexBuffer.GetColors(),
		[](const Durin::FVector4f& Color) {
			return Color != Durin::FVector4f(1.0f);
		}));
	Durin::FStaticMeshRenderData* MutableLODContractRenderData =
		Durin::FStaticMeshTestAccess::GetMutableRenderData(LODContractMesh);
	MutableLODContractRenderData->LODResources.push_back(
		MutableLODContractRenderData->LODResources[0]);
	MutableLODContractRenderData->LODResources[0].ScreenSize = 1.0f;
	Durin::FStaticMeshLODResources& ReducedLOD =
		MutableLODContractRenderData->LODResources[1];
	ReducedLOD.ScreenSize = 0.0f;
	ReducedLOD.Sections.resize(1);
	ReducedLOD.Sections[0].FirstIndex = 0;
	ReducedLOD.Sections[0].IndexCount = 3;
	MutableLODContractRenderData->LODVertexFactories.resize(2);
	MutableLODContractRenderData->RecalculateBounds();

	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

	const size_t InitialRenderResourceCount =
		Durin::GetNumInitializedRenderResources();
	Durin::DStaticMesh* LifecycleMesh =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::AddToRoot(LifecycleMesh);
	const Durin::FStaticMeshRenderResourceStatus InitialLifecycleStatus =
		LifecycleMesh->GetRenderResourceStatus();
	EXPECT_EQ(
		InitialLifecycleStatus.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Unavailable);
	EXPECT_NE(InitialLifecycleStatus.Revision, 0u);
	EXPECT_FALSE(LifecycleMesh->GetLOD0LocalBounds().has_value());
	const auto StaticMeshInitStart =
		std::chrono::steady_clock::now();
	LifecycleMesh->InitResources();
	const Durin::FStaticMeshRenderResourceStatus QueuedLifecycleStatus =
		LifecycleMesh->GetRenderResourceStatus();
	EXPECT_EQ(
		QueuedLifecycleStatus.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Queued);
	EXPECT_GT(
		QueuedLifecycleStatus.Revision,
		InitialLifecycleStatus.Revision);
	LifecycleMesh->InitResources();
	EXPECT_EQ(
		LifecycleMesh->GetRenderResourceStatus().Revision,
		QueuedLifecycleStatus.Revision);
	Durin::FlushRenderingCommands();
	const Durin::FStaticMeshRenderResourceStatus ReadyLifecycleStatus =
		LifecycleMesh->GetRenderResourceStatus();
	EXPECT_TRUE(ReadyLifecycleStatus.IsReady());
	EXPECT_GT(
		ReadyLifecycleStatus.Revision,
		QueuedLifecycleStatus.Revision);
	const auto StaticMeshInitMicroseconds =
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - StaticMeshInitStart)
			.count();
	ASSERT_NE(LifecycleMesh->GetRenderData(), nullptr);
	ASSERT_EQ(
		LifecycleMesh->GetRenderData()->LODVertexFactories.size(),
		LifecycleMesh->GetRenderData()->LODResources.size());
	const Durin::FStaticMeshLODResources& LifecycleLOD =
		LifecycleMesh->GetRenderData()->LODResources[0];
	const Durin::FLocalVertexFactory& LifecycleVertexFactory =
		LifecycleMesh->GetRenderData()
			->LODVertexFactories[0].VertexFactory;
	ASSERT_TRUE(LifecycleVertexFactory.IsReady());
	ASSERT_NE(LifecycleVertexFactory.GetDeclaration(), nullptr);
	ASSERT_EQ(LifecycleVertexFactory.GetStreams().size(), 4u);
	EXPECT_EQ(
		LifecycleVertexFactory.GetStreams()[0].VertexBuffer,
		LifecycleLOD.VertexBuffers.PositionVertexBuffer.GetRHI());
	EXPECT_EQ(
		LifecycleVertexFactory.GetStreams()[1].VertexBuffer,
		LifecycleLOD.VertexBuffers.StaticMeshVertexBuffer
			.TangentsVertexBuffer.GetRHI());
	EXPECT_EQ(
		LifecycleVertexFactory.GetStreams()[2].VertexBuffer,
		LifecycleLOD.VertexBuffers.StaticMeshVertexBuffer
			.TexCoordVertexBuffer.GetRHI());
	EXPECT_EQ(
		LifecycleVertexFactory.GetStreams()[3].VertexBuffer,
		LifecycleLOD.VertexBuffers.ColorVertexBuffer.GetRHI());
	EXPECT_TRUE(std::ranges::none_of(
		LifecycleVertexFactory.GetStreams(),
		[&LifecycleLOD](const Durin::FVertexInputStream& Stream) {
			return Stream.VertexBuffer == LifecycleLOD.IndexBuffer.GetRHI();
		}));
	const auto StaticMeshBufferBytes =
		LifecycleLOD.VertexBuffers.PositionVertexBuffer.GetRHI()->GetSize()
		+ LifecycleLOD.VertexBuffers.StaticMeshVertexBuffer
			.TangentsVertexBuffer.GetRHI()->GetSize()
		+ LifecycleLOD.VertexBuffers.StaticMeshVertexBuffer
			.TexCoordVertexBuffer.GetRHI()->GetSize()
		+ LifecycleLOD.VertexBuffers.ColorVertexBuffer.GetRHI()->GetSize()
		+ LifecycleLOD.IndexBuffer.GetRHI()->GetSize();
	const auto ExpectedStaticMeshBufferBytes =
		LifecycleLOD.GetNumVertices()
			* (sizeof(Durin::FVector3f)
				+ sizeof(Durin::FStaticMeshPackedTangentBasis)
				+ sizeof(Durin::FStaticMeshTexcoordVertex)
				+ sizeof(Durin::FStaticMeshColorVertex))
		+ LifecycleLOD.GetNumIndices() * sizeof(Durin::uint32);
	EXPECT_EQ(
		StaticMeshBufferBytes,
		ExpectedStaticMeshBufferBytes);
	std::cout
		<< "[StaticMeshLODResourcesMetric] init_us="
		<< StaticMeshInitMicroseconds
		<< " buffers=5 bytes=" << StaticMeshBufferBytes
		<< " vertex_factory_streams="
		<< LifecycleVertexFactory.GetStreams().size()
		<< " indexed_draws_per_section=1\n";
	EXPECT_EQ(
		LifecycleMesh->GetRenderData()->GetNumInitializedResources(),
		6u);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 6u);

	const Durin::FStaticMeshRenderData* OriginalRenderData =
		LifecycleMesh->GetRenderData();
	Durin::DStaticMesh* ReplacementCandidate =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::AddToRoot(ReplacementCandidate);
	const Durin::FStaticMeshRenderData* ReplacementRenderData =
		ReplacementCandidate->GetRenderData();
	const Durin::FStaticMeshRenderResourceStatus CandidateStatusBeforeExchange =
		ReplacementCandidate->GetRenderResourceStatus();
	std::string ReplacementError;
	ASSERT_TRUE(LifecycleMesh->ExchangeImportedState(
		*ReplacementCandidate, ReplacementError))
		<< ReplacementError;
	const Durin::FStaticMeshRenderResourceStatus TargetStatusAfterExchange =
		LifecycleMesh->GetRenderResourceStatus();
	const Durin::FStaticMeshRenderResourceStatus CandidateStatusAfterExchange =
		ReplacementCandidate->GetRenderResourceStatus();
	EXPECT_TRUE(TargetStatusAfterExchange.IsReady());
	EXPECT_GT(
		TargetStatusAfterExchange.Revision,
		ReadyLifecycleStatus.Revision);
	EXPECT_EQ(
		CandidateStatusAfterExchange.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Unavailable);
	EXPECT_GT(
		CandidateStatusAfterExchange.Revision,
		CandidateStatusBeforeExchange.Revision);
	EXPECT_EQ(LifecycleMesh->GetRenderData(), ReplacementRenderData);
	EXPECT_EQ(ReplacementCandidate->GetRenderData(), OriginalRenderData);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 6u);

	const Durin::FStaticMeshRenderResourceStatus TargetStatusBeforeReverse =
		LifecycleMesh->GetRenderResourceStatus();
	const Durin::FStaticMeshRenderResourceStatus CandidateStatusBeforeReverse =
		ReplacementCandidate->GetRenderResourceStatus();
	ASSERT_TRUE(LifecycleMesh->ExchangeImportedState(
		*ReplacementCandidate, ReplacementError))
		<< ReplacementError;
	EXPECT_GT(
		LifecycleMesh->GetRenderResourceStatus().Revision,
		TargetStatusBeforeReverse.Revision);
	EXPECT_GT(
		ReplacementCandidate->GetRenderResourceStatus().Revision,
		CandidateStatusBeforeReverse.Revision);
	EXPECT_EQ(LifecycleMesh->GetRenderData(), OriginalRenderData);
	EXPECT_EQ(
		ReplacementCandidate->GetRenderData(),
		ReplacementRenderData);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 6u);

	Durin::DStaticMesh* FailedReplacementCandidate =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::AddToRoot(FailedReplacementCandidate);
	Durin::FStaticMeshTestAccess::GetMutableRenderData(
		FailedReplacementCandidate)
		->LODResources.push_back(
			FailedReplacementCandidate->GetRenderData()
				->LODResources[0]);
	Durin::FStaticMeshTestAccess::GetMutableRenderData(
		FailedReplacementCandidate)
		->LODResources[0].ScreenSize = 0.5f;
	Durin::FStaticMeshTestAccess::GetMutableRenderData(
		FailedReplacementCandidate)
		->LODResources[1].Sections[0].MaterialSlotIndex = 99;
	const Durin::FStaticMeshRenderResourceStatus TargetStatusBeforeFailure =
		LifecycleMesh->GetRenderResourceStatus();
	const Durin::FStaticMeshRenderResourceStatus CandidateStatusBeforeFailure =
		FailedReplacementCandidate->GetRenderResourceStatus();
	EXPECT_FALSE(LifecycleMesh->ExchangeImportedState(
		*FailedReplacementCandidate, ReplacementError));
	EXPECT_EQ(
		LifecycleMesh->GetRenderResourceStatus().Revision,
		TargetStatusBeforeFailure.Revision);
	EXPECT_EQ(
		FailedReplacementCandidate->GetRenderResourceStatus().Revision,
		CandidateStatusBeforeFailure.Revision);
	EXPECT_EQ(LifecycleMesh->GetRenderData(), OriginalRenderData);
	EXPECT_EQ(
		FailedReplacementCandidate->GetRenderData()
			->GetNumInitializedResources(),
		0u);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 6u);

	Durin::DStaticMesh* InvalidMesh =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::AddToRoot(InvalidMesh);
	Durin::FStaticMeshTestAccess::GetMutableRenderData(InvalidMesh)
		->LODResources.push_back(
			InvalidMesh->GetRenderData()->LODResources[0]);
	Durin::FStaticMeshTestAccess::GetMutableRenderData(InvalidMesh)
		->LODResources[0].ScreenSize = 0.5f;
	Durin::FStaticMeshTestAccess::GetMutableRenderData(InvalidMesh)
		->LODResources[1].Sections[0].MaterialSlotIndex = 99;
	const Durin::FStaticMeshRenderResourceStatus InvalidInitialStatus =
		InvalidMesh->GetRenderResourceStatus();
	InvalidMesh->InitResources();
	const Durin::FStaticMeshRenderResourceStatus InvalidQueuedStatus =
		InvalidMesh->GetRenderResourceStatus();
	EXPECT_EQ(
		InvalidQueuedStatus.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Queued);
	EXPECT_GT(InvalidQueuedStatus.Revision, InvalidInitialStatus.Revision);
	Durin::FlushRenderingCommands();
	const Durin::FStaticMeshRenderResourceStatus InvalidFailedStatus =
		InvalidMesh->GetRenderResourceStatus();
	EXPECT_EQ(
		InvalidFailedStatus.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Failed);
	EXPECT_GT(InvalidFailedStatus.Revision, InvalidQueuedStatus.Revision);
	EXPECT_EQ(
		InvalidMesh->GetRenderData()->GetNumInitializedResources(),
		0u);

	struct FBlockedRenderCommandState
	{
		std::mutex Mutex;
		std::condition_variable CV;
		bool bContinue = false;
	};
	auto BlockedRenderCommand =
		std::make_shared<FBlockedRenderCommandState>();
	struct FBlockStaticMeshRelease
	{
		static constexpr auto GetName() -> const char*
		{
			return "BlockStaticMeshRelease";
		}
	};
	Durin::EnqueueRenderCommand<FBlockStaticMeshRelease>(
		[BlockedRenderCommand](Durin::FRHICommandListImmediate&) {
			std::unique_lock Lock(BlockedRenderCommand->Mutex);
			BlockedRenderCommand->CV.wait(
				Lock, [&] { return BlockedRenderCommand->bContinue; });
		});
	const Durin::FObjectHandle LifecycleHandle =
		Durin::MakeObjectHandle(LifecycleMesh);
	const Durin::FStaticMeshRenderResourceStatus StatusBeforeRelease =
		LifecycleMesh->GetRenderResourceStatus();
	ASSERT_TRUE(StatusBeforeRelease.IsReady());
	Durin::RemoveFromRoot(LifecycleMesh);
	Durin::MarkAsGarbage(LifecycleMesh);
	Durin::CollectGarbage();
	const Durin::FStaticMeshRenderResourceStatus ReleaseQueuedStatus =
		LifecycleMesh->GetRenderResourceStatus();
	EXPECT_EQ(
		ReleaseQueuedStatus.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Unavailable);
	EXPECT_GT(ReleaseQueuedStatus.Revision, StatusBeforeRelease.Revision);
	EXPECT_NE(Durin::ResolveObjectHandle(LifecycleHandle), nullptr);
	EXPECT_FALSE(LifecycleMesh->IsReadyForFinishDestroy());
	{
		std::lock_guard Lock(BlockedRenderCommand->Mutex);
		BlockedRenderCommand->bContinue = true;
	}
	BlockedRenderCommand->CV.notify_all();
	Durin::FlushRenderingCommands();
	const Durin::FStaticMeshRenderResourceStatus ReleasedStatus =
		LifecycleMesh->GetRenderResourceStatus();
	EXPECT_EQ(
		ReleasedStatus.Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Unavailable);
	EXPECT_GT(ReleasedStatus.Revision, ReleaseQueuedStatus.Revision);
	EXPECT_TRUE(LifecycleMesh->IsReadyForFinishDestroy());
	EXPECT_EQ(
		LifecycleMesh->GetRenderData()->GetNumInitializedResources(),
		0u);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(LifecycleHandle), nullptr);

	Durin::RemoveFromRoot(InvalidMesh);
	Durin::MarkAsGarbage(InvalidMesh);
	Durin::RemoveFromRoot(FailedReplacementCandidate);
	Durin::MarkAsGarbage(FailedReplacementCandidate);
	Durin::RemoveFromRoot(ReplacementCandidate);
	Durin::MarkAsGarbage(ReplacementCandidate);
	Durin::CollectGarbage();
	Durin::FlushRenderingCommands();
	Durin::CollectGarbage();
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount);

	Durin::DStaticMesh* ReloadedMesh = nullptr;
	const Durin::Asset::FAssetResult ReloadMeshResult =
		Durin::Asset::LoadAsset(MeshPath, ReloadedMesh);
	ASSERT_TRUE(ReloadMeshResult) << ReloadMeshResult.Message;
	ASSERT_FALSE(ReloadedMesh->GetDerivedDataDiagnostic().bSourceImporterInvoked);
	const Durin::FStaticMeshMaterialSlotDefinition* Slot =
		ReloadedMesh->GetMaterialSlot(0);
	ASSERT_NE(Slot, nullptr);
	auto* ReloadedMaterial =
		Durin::Cast<Durin::DMaterialInstance>(Slot->DefaultMaterial.Get());
	ASSERT_NE(ReloadedMaterial, nullptr);
	Durin::DTexture2D* ReloadedTexture = nullptr;
	ASSERT_TRUE(ReloadedMaterial->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ReloadedTexture));
	ASSERT_NE(ReloadedTexture, nullptr);
	EXPECT_TRUE(ReloadedTexture->IsSRGB());
	Durin::FVector3 ImportedFactor;
	ASSERT_TRUE(ReloadedMaterial->GetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), ImportedFactor));
	EXPECT_EQ(ImportedFactor, Durin::FVector3(0.5, 0.75, 0.25));

	struct FBeginSceneImportFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginSceneImportFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginSceneImportFrame>(
		[](Durin::FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
		});

	FSceneImportRenderEngine Engine;
	Durin::FRendererModule Renderer;
	Renderer.StartupModule();
	Engine.SetRenderer(&Renderer);
	Durin::GEngine = &Engine;
	Durin::DMaterialInstance* TextureOnly =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "TextureOnlyControl");
	Durin::DMaterialInstance* FactorOnly =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FactorOnlyControl");
	Durin::DMaterialInstance* FailedResourceMaterial =
		Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, "FailedResourceControl");
	ASSERT_TRUE(TextureOnly->SetParent(ReloadedMaterial->GetParent()));
	ASSERT_TRUE(TextureOnly->SetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ReloadedTexture));
	ASSERT_TRUE(TextureOnly->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(1.0, 1.0, 1.0)));
	ASSERT_TRUE(FactorOnly->SetParent(ReloadedMaterial->GetParent()));
	ASSERT_TRUE(FactorOnly->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), ImportedFactor));
	ASSERT_TRUE(FailedResourceMaterial->SetParent(
		ReloadedMaterial->GetParent()));
	Durin::FMaterialSamplerState FailedSampler;
	FailedSampler.AddressU =
		Durin::EMaterialSamplerAddressMode::ClampToEdge;
	FailedSampler.AddressV =
		Durin::EMaterialSamplerAddressMode::ClampToEdge;
	ASSERT_TRUE(FailedResourceMaterial->SetScalarParameterValue(
		Durin::FName("BaseColorSamplerState"),
		Durin::EncodeMaterialSamplerState(FailedSampler)));
	Durin::FlushRenderingCommands();

	Durin::FRenderedAssetThumbnailVisualContract Contract;
	Contract.Output.Width = 64;
	Contract.Output.Height = 64;
	{
		Durin::Tests::FRenderedAssetThumbnailTestPool Pool(Contract);
		ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
		std::vector<Durin::FViewRenderCounters> CounterSnapshots;
		FScopedSceneImportCounterSink CounterSink(CounterSnapshots);
		auto Capture = [&](
			Durin::DStaticMesh* Mesh,
			Durin::DMaterialInterface* Material,
			bool bForceLOD0 = false) {
			std::vector<Durin::uint8> Pixels;
			Pool.SetForceLOD0(bForceLOD0);
			EXPECT_TRUE(Pool.SetMaterial(
				Mesh, Material, Durin::FTransform(), Error)) << Error;
			EXPECT_TRUE(Pool.BeginCapture(Error, false)) << Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.PollCapture(Pixels, Error),
				Durin::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			Pool.Reset();
			return Pixels;
		};

		const std::vector<Durin::uint8> ImportedPixels =
			Capture(ReloadedMesh, ReloadedMaterial);
		const std::vector<Durin::uint8> TextureOnlyPixels =
			Capture(ReloadedMesh, TextureOnly);
		const std::vector<Durin::uint8> FactorOnlyPixels =
			Capture(ReloadedMesh, FactorOnly);
		const std::vector<Durin::uint8> AutomaticLODPixels =
			Capture(LODContractMesh, ReloadedMaterial);
		const std::vector<Durin::uint8> ForcedLOD0Pixels =
			Capture(LODContractMesh, ReloadedMaterial, true);
		ASSERT_EQ(ImportedPixels.size(), 64u * 64u * 4u);
		ASSERT_EQ(TextureOnlyPixels.size(), ImportedPixels.size());
		ASSERT_EQ(FactorOnlyPixels.size(), ImportedPixels.size());
		const size_t Center = (32u * 64u + 32u) * 4u;
		EXPECT_GT(ImportedPixels[Center + 3], 0u);
		EXPECT_GT(ImportedPixels[Center + 2], ImportedPixels[Center]);
		EXPECT_GT(ImportedPixels[Center], ImportedPixels[Center + 1]);
		EXPECT_NE(ImportedPixels, TextureOnlyPixels);
		EXPECT_NE(ImportedPixels, FactorOnlyPixels);
		ASSERT_EQ(
			AutomaticLODPixels.size(),
			ImportedPixels.size());
		ASSERT_EQ(ForcedLOD0Pixels.size(), ImportedPixels.size());
		EXPECT_NE(AutomaticLODPixels, ImportedPixels);
		EXPECT_NE(AutomaticLODPixels, ForcedLOD0Pixels);
		EXPECT_EQ(
			Durin::FXxHash128::HashBuffer(
				AutomaticLODPixels).ToString(),
			"36ff62c3dd2df3cd3cf45db46e9e4198");
		EXPECT_EQ(
			Durin::FXxHash128::HashBuffer(
				ForcedLOD0Pixels).ToString(),
			"bdd34099da4b080de210ad2d9af122a9");
		Durin::VulkanRHI::ArmVulkanCreateFailure(
			Durin::VulkanRHI::EVulkanCreateFailurePoint::Sampler);
		const std::vector<Durin::uint8> FailedResourcePixels =
			Capture(ReloadedMesh, FailedResourceMaterial);
		ASSERT_EQ(FailedResourcePixels.size(), ImportedPixels.size());
		ASSERT_EQ(CounterSnapshots.size(), 6u);
		const std::array<size_t, 5> ExpectedSections{1u, 1u, 1u, 1u, 4u};
		for (size_t Index = 0; Index < ExpectedSections.size(); ++Index)
		{
			const Durin::FViewRenderCounters& Counters =
				CounterSnapshots[Index];
			EXPECT_EQ(Counters.VisibleStaticMeshCandidates, 1u);
			EXPECT_EQ(Counters.PreparedStaticMeshPrimitives, 1u);
			EXPECT_EQ(
				Counters.PreparedStaticMeshSections, ExpectedSections[Index]);
			EXPECT_EQ(
				Counters.OpaqueStaticMeshSections, ExpectedSections[Index]);
			EXPECT_EQ(Counters.OpaqueStaticMeshStateGroups, 1u);
			EXPECT_EQ(Counters.StaticMeshResourceAttemptedDraws,
				ExpectedSections[Index]);
			EXPECT_EQ(Counters.StaticMeshResourceSuccessfulDraws,
				ExpectedSections[Index]);
			EXPECT_EQ(Counters.StaticMeshResourceRejectedDraws, 0u);
			EXPECT_EQ(
				Counters.StaticMeshAttemptedDraws, ExpectedSections[Index]);
			EXPECT_EQ(
				Counters.StaticMeshSuccessfulDraws, ExpectedSections[Index]);
			EXPECT_EQ(Counters.StaticMeshRejectedDraws, 0u);
		}
		const Durin::FViewRenderCounters& FailedResourceCounters =
			CounterSnapshots.back();
		EXPECT_EQ(FailedResourceCounters.PreparedStaticMeshSections, 1u);
		EXPECT_EQ(
			FailedResourceCounters.StaticMeshResourceAttemptedDraws, 1u);
		EXPECT_EQ(
			FailedResourceCounters.StaticMeshResourceSuccessfulDraws, 0u);
		EXPECT_EQ(
			FailedResourceCounters.StaticMeshResourceRejectedDraws, 1u);
		EXPECT_EQ(FailedResourceCounters.StaticMeshAttemptedDraws, 1u);
		EXPECT_EQ(FailedResourceCounters.StaticMeshSuccessfulDraws, 0u);
		EXPECT_EQ(FailedResourceCounters.StaticMeshRejectedDraws, 1u);

		struct FEndSceneImportFrame
		{
			static constexpr auto GetName() -> const char* { return "EndSceneImportFrame"; }
		};
		Durin::EnqueueRenderCommand<FEndSceneImportFrame>(
			[](Durin::FRHICommandListImmediate& CommandList) {
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		Durin::FlushRenderingCommands();
	}

	Durin::GEngine = nullptr;
	auto* Sphere = Durin::Cast<Durin::DStaticMesh>(PreloadedSphere.Get());
	ASSERT_NE(Sphere, nullptr);
	EXPECT_GT(
		ReloadedMesh->GetRenderData()->GetNumInitializedResources(),
		0u);
	EXPECT_GT(
		LODContractMesh->GetRenderData()->GetNumInitializedResources(),
		0u);
	EXPECT_EQ(Sphere->GetRenderData()->GetNumInitializedResources(), 0u);
	Durin::MarkAsGarbage(FactorOnly);
	Durin::MarkAsGarbage(TextureOnly);
	Durin::MarkAsGarbage(FailedResourceMaterial);
	PreloadedSphere = {};
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LODContractPath));
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MeshPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MaterialPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(TexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StandardPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(LODContractPath));
	Renderer.ShutdownModule();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
	Durin::Testing::RemoveTestWorkDirectory(Root);
}
