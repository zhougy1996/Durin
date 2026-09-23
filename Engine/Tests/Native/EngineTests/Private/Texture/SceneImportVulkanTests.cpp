#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "Threading/Task.h"
#include "NativeAssetTestSupport.h"
#include "Asset/Testing.h"
#include "VulkanEngineTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetRetention.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/DObjectArray.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Rendering/PrimitiveSceneProxy.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Image/ImageEncoder.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Renderers/SceneVisibility.h"
#include "StaticMesh/StaticMesh.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMeshTestAccess.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/AssetThumbnailTestFixtures.h"
#include "Texture/Texture2D.h"
#include "TextureTestSupport.h"
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
	auto SaveImportedFunctionBaseline(std::string_view Name,
		const Durin::FByteBuffer& Pixels) -> void
	{
		Durin::FByteBuffer Png;
		ASSERT_TRUE(Durin::Image::EncodeRgba8Png(Pixels, 64, 64, Png));
		const auto Directory = Durin::Testing::GetTestWorkDirectory()
			/ "ReusableMaterialFunctions";
		std::filesystem::create_directories(Directory);
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Png,
			Directory / (std::string(Name) + ".png")));
	}

	std::vector<Durin::FViewRenderTelemetry>* GSceneImportTelemetrySnapshots = nullptr;

	auto CaptureSceneImportTelemetrySnapshot(
		const Durin::FViewRenderTelemetry& Telemetry) -> void
	{
		if (GSceneImportTelemetrySnapshots != nullptr)
		{
			GSceneImportTelemetrySnapshots->push_back(Telemetry);
		}
	}

	class FScopedSceneImportTelemetrySink final
	{
	public:
		explicit FScopedSceneImportTelemetrySink(
			std::vector<Durin::FViewRenderTelemetry>& Snapshots)
		{
			GSceneImportTelemetrySnapshots = &Snapshots;
			Durin::SetViewRenderTelemetrySink(CaptureSceneImportTelemetrySnapshot);
		}

		~FScopedSceneImportTelemetrySink()
		{
			Durin::SetViewRenderTelemetrySink(nullptr);
			GSceneImportTelemetrySnapshots = nullptr;
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

	auto MakeAssetPath(std::string_view Value) -> Durin::FPackagePath
	{
		Durin::FPackagePath Result;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Result));
		return Result;
	}
}

TEST(FSceneImportVulkanTests, RendersReloadedSrgbTextureAndBaseColorFactor)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	Durin::Testing::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	Durin::FObjectPath SpherePath;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(
		Durin::Editor::FThumbnailVisualContract::SphereAssetPath, SpherePath));
	Durin::Editor::FRetainedAsset PreloadedSphere;
	std::string Error;
	ASSERT_TRUE(Durin::Editor::FAssetRetentionService::Acquire(
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
		"ThumbnailStudioCube.dasset"})
	{
		std::filesystem::copy_file(
			BuiltInEnvironmentRoot / File,
			Root / "Engine/Content/Renderer" / File,
			std::filesystem::copy_options::overwrite_existing);
	}
	const std::array Mounts{
		Durin::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::EMountOwner::Test,
			.Root = Root / "Engine/Content",
			.bAutoScan = true,
			.bContentWritable = true},
		Durin::FMountPoint{
			.VirtualRoot = "/SceneImportVulkan/",
			.Owner = Durin::EMountOwner::Test,
			.Root = Root / "Project/Content",
			.bAutoScan = true,
			.bContentWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::Testing::FScopedMountRegistryFixture MountFixture(Mounts);
	ASSERT_TRUE(MountFixture.IsValid()) << MountFixture.GetError();
	// Replace catalog paths captured from the default mounts before mutating test assets.
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	Durin::FPackagePath StandardPath;

	const Durin::FPackagePath DestinationDirectory =
		MakeAssetPath("/SceneImportVulkan/Imports/RenderedOpaque");
	const std::filesystem::path MountedScene =
		Root / "Project/Content/Models/RenderedOpaqueDataUri.gltf";
	std::filesystem::create_directories(MountedScene.parent_path());
	std::filesystem::copy_file(
		std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "StaticModelMaterials/RenderedOpaqueDataUri.gltf",
		MountedScene,
		std::filesystem::copy_options::overwrite_existing);
	Durin::AssetForge::Builtins::FSceneImportResult Executed;
	{
		using namespace Durin::AssetForge::Builtins;
		FSceneImportSession Session(MountedScene.generic_string(), DestinationDirectory,
			Durin::FStaticMeshImportSettings::MakeDurin());
		const auto AdvanceTo = [&](ESceneImportPhase Phase) {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (std::chrono::steady_clock::now() < Deadline)
			{
				Durin::PumpGameThreadDeferredWork();
				Durin::FAssetCompilingManager::Get().ProcessAsyncTasks(false);
				Session.Tick();
				if (Session.GetProgress().Phase == Phase) return true;
				if (Session.GetProgress().Phase == ESceneImportPhase::Completed) return false;
				std::this_thread::yield();
			}
			return false;
		};
		ASSERT_TRUE(AdvanceTo(ESceneImportPhase::Ready)) << Session.GetResult().Message;
		ASSERT_TRUE(Session.PreviewMaterials(DestinationDirectory, {}).bSucceeded);
		ASSERT_TRUE(Session.BeginImport(DestinationDirectory, {}));
		ASSERT_TRUE(AdvanceTo(ESceneImportPhase::Completed)) << Session.GetResult().Message;
		Executed = Session.GetResult();
	}
	ASSERT_TRUE(Executed)
		<< Executed.Message;
	ASSERT_EQ(Executed.Outputs.size(), 3u);
	Durin::FPackagePath MeshPath;
	Durin::FPackagePath TexturePath;
	Durin::FPackagePath MaterialPath;
	for (const Durin::AssetForge::FImportOutputSummary& Output
		: Executed.Outputs)
	{
		if (Output.AssetClassName
			== Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString())
			MeshPath = Output.AssetPath;
		else if (Output.AssetClassName
			== Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString())
			TexturePath = Output.AssetPath;
		else if (Output.AssetClassName
			== Durin::DMaterial::StaticClass()->GetQualifiedName().ToString())
			MaterialPath = Output.AssetPath;
	}
	ASSERT_TRUE(MeshPath.IsValid());
	ASSERT_TRUE(TexturePath.IsValid());
	ASSERT_TRUE(MaterialPath.IsValid());
	Durin::DMaterial* LiveMaterial = nullptr;
	{
		auto LoadedValue = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		LiveMaterial = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(LiveMaterial, nullptr);
	EXPECT_EQ(LiveMaterial->GetParent(), nullptr);
	StandardPath = MaterialPath;
	EXPECT_TRUE(StandardPath.GetView().starts_with(DestinationDirectory.ToString() + "/Materials/M_"));
	ASSERT_TRUE(Durin::FindAssetExact(StandardPath));
	Durin::DTexture2D* LiveTexture = nullptr;
	ASSERT_TRUE(LiveMaterial->GetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), LiveTexture));
	ASSERT_NE(LiveTexture, nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::UnloadPackage(TexturePath));

	const Durin::FPackagePath LODContractPath =
		MakeAssetPath("/SceneImportVulkan/LODContract");
	const Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> LODContractImport =
		Durin::AssetForge::Builtins::ImportStaticMeshForTest(
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
	EXPECT_TRUE(LifecycleMesh->GetLOD0LocalBounds().has_value());
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
		+ LifecycleLOD.GetNumIndices() * sizeof(uint32);
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
	const Durin::FObjectKey LifecycleHandle =
		Durin::FObjectKey(LifecycleMesh);
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
	EXPECT_EQ(Durin::ResolveObjectKey(LifecycleHandle), nullptr);
	EXPECT_NE(Durin::GDObjectArray.Resolve(LifecycleHandle), nullptr);
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
	EXPECT_EQ(Durin::ResolveObjectKey(LifecycleHandle), nullptr);

	Durin::RemoveFromRoot(InvalidMesh);
	Durin::MarkAsGarbage(InvalidMesh);
	Durin::CollectGarbage();
	Durin::FlushRenderingCommands();
	Durin::CollectGarbage();
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount);

	Durin::DStaticMesh* ReloadedMesh = nullptr;
	const auto ReloadMeshResult =
		Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath));
		ReloadedMesh = ReloadMeshResult.value_or(nullptr);
	ASSERT_TRUE(ReloadMeshResult) << (ReloadMeshResult ? std::string{} : ReloadMeshResult.error().Message);
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*ReloadedMesh);
	ASSERT_NE(ReloadedMesh->GetRenderData(), nullptr);
	const Durin::FMeshMaterialSlotDefinition* Slot =
		ReloadedMesh->GetMaterialSlot(0);
	ASSERT_NE(Slot, nullptr);
	auto* ReloadedMaterial =
		Durin::Cast<Durin::DMaterial>(Slot->DefaultMaterial.Get());
	ASSERT_NE(ReloadedMaterial, nullptr);
	Durin::DTexture2D* ReloadedTexture = nullptr;
	ASSERT_TRUE(ReloadedMaterial->GetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), ReloadedTexture));
	ASSERT_NE(ReloadedTexture, nullptr);
	// Authored texture reload rebuilds platform data asynchronously before upload.
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*ReloadedTexture);
	const auto TextureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (ReloadedTexture->IsResourceUpdatePending() && std::chrono::steady_clock::now() < TextureDeadline)
	{
		Durin::PumpGameThreadDeferredWork();
		std::this_thread::yield();
	}
	ASSERT_TRUE(ReloadedTexture->HasUsableResource()) << ReloadedTexture->GetResourceUpdateError();
	EXPECT_TRUE(ReloadedTexture->IsSRGB());
	Durin::FVector3 ImportedFactor;
	ASSERT_TRUE(ReloadedMaterial->GetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), ImportedFactor));
	EXPECT_EQ(ImportedFactor, Durin::FVector3(0.5, 0.75, 0.25));
	auto* ReloadedParentMaterial =
		ReloadedMaterial;
	ASSERT_NE(ReloadedParentMaterial, nullptr);
	Durin::DObject* ReloadedParentCompilationObject = ReloadedParentMaterial;
	Durin::FAssetCompilingManager::Get().FinishCompilationForObjects(
		std::span<Durin::DObject* const>(&ReloadedParentCompilationObject, 1));
	ASSERT_TRUE(ReloadedParentMaterial->GetMaterialCompileStatus().IsCurrent());

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
	Durin::FModuleTestHarness RendererLifecycle("SceneImportRendererTest");
	RendererLifecycle.Start(Renderer);
	Engine.SetRenderer(&Renderer);
	Durin::GEngine = &Engine;
	Durin::DMaterialInstance* TextureOnly =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "TextureOnlyControl");
	Durin::DMaterialInstance* FactorOnly =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FactorOnlyControl");
	Durin::DMaterialInstance* FailedResourceMaterial =
		Durin::NewObject<Durin::DMaterialInstance>(
			nullptr, "FailedResourceControl");
	ASSERT_TRUE(TextureOnly->SetParent(ReloadedMaterial));
	ASSERT_TRUE(TextureOnly->SetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), ReloadedTexture));
	ASSERT_TRUE(TextureOnly->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(1.0, 1.0, 1.0)));
	ASSERT_TRUE(FactorOnly->SetParent(ReloadedMaterial));
	ASSERT_TRUE(FactorOnly->SetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), nullptr));
	ASSERT_TRUE(FactorOnly->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), ImportedFactor));
	ASSERT_TRUE(FailedResourceMaterial->SetParent(
		ReloadedMaterial));
	Durin::FMaterialSamplerState FailedSampler;
	FailedSampler.AddressU =
		Durin::EMaterialSamplerAddressMode::ClampToEdge;
	FailedSampler.AddressV =
		Durin::EMaterialSamplerAddressMode::ClampToEdge;
	ASSERT_TRUE(FailedResourceMaterial->SetParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(
			Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture,
		Durin::FMaterialParameterValue::MakeTexture(nullptr, FailedSampler)));
	const std::array<Durin::DObject*, 3> Controls{TextureOnly, FactorOnly, FailedResourceMaterial};
	Durin::FAssetCompilingManager::Get().FinishCompilationForObjects(Controls);
	for (auto* Object : Controls)
		ASSERT_TRUE(Durin::Cast<Durin::DMaterialInstance>(Object)->GetMaterialCompileStatus().IsCurrent());

	Durin::FlushRenderingCommands();

	Durin::Editor::FThumbnailVisualContract Contract;
	Contract.Output.Width = 64;
	Contract.Output.Height = 64;
	{
		Durin::Tests::FAssetThumbnailTestPool Pool(Contract);
		ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
		std::vector<Durin::FViewRenderTelemetry> TelemetrySnapshots;
		FScopedSceneImportTelemetrySink TelemetrySink(TelemetrySnapshots);
		auto Capture = [&](
			Durin::DStaticMesh* Mesh,
			Durin::DMaterialInterface* Material,
			bool bForceLOD0 = false,
			Durin::Editor::EThumbnailCaptureState ExpectedState =
				Durin::Editor::EThumbnailCaptureState::Ready) {
			Durin::FByteBuffer Pixels;
			Pool.SetForceLOD0(bForceLOD0);
			EXPECT_TRUE(Pool.SetMaterial(
				Mesh, Material, Durin::FTransform()));
			EXPECT_TRUE(Pool.BeginCapture(false));
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.FinishCapture(Pixels, Error),
				ExpectedState) << Error;
			Pool.Reset();
			return Pixels;
		};

		const Durin::FByteBuffer ImportedPixels =
			Capture(ReloadedMesh, ReloadedMaterial);
		const Durin::FByteBuffer TextureOnlyPixels =
			Capture(ReloadedMesh, TextureOnly);
		const Durin::FByteBuffer FactorOnlyPixels =
			Capture(ReloadedMesh, FactorOnly);
		const Durin::FByteBuffer AutomaticLODPixels =
			Capture(LODContractMesh, ReloadedMaterial);
		const Durin::FByteBuffer ForcedLOD0Pixels =
			Capture(LODContractMesh, ReloadedMaterial, true);
		ASSERT_EQ(ImportedPixels.size(), 64u * 64u * 4u);
		ASSERT_EQ(TextureOnlyPixels.size(), ImportedPixels.size());
		ASSERT_EQ(FactorOnlyPixels.size(), ImportedPixels.size());
		SaveImportedFunctionBaseline("imported-texture-and-factor", ImportedPixels);
		SaveImportedFunctionBaseline("imported-texture-only", TextureOnlyPixels);
		SaveImportedFunctionBaseline("imported-factor-only", FactorOnlyPixels);
		SaveImportedFunctionBaseline("imported-automatic-lod", AutomaticLODPixels);
		SaveImportedFunctionBaseline("imported-forced-lod0", ForcedLOD0Pixels);
		const size_t Center = (32u * 64u + 32u) * 4u;
		EXPECT_GT(std::to_integer<uint8>(ImportedPixels[Center + 3]), 0u);
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
		// Compare complete images within the same GPU run. Exact cross-device
		// lighting hashes are not a portable LOD contract; section counts below
		// independently verify automatic selection and the forced LOD 0 route.
		EXPECT_EQ(AutomaticLODPixels, Capture(LODContractMesh, ReloadedMaterial));
		EXPECT_EQ(ForcedLOD0Pixels, Capture(LODContractMesh, ReloadedMaterial, true));
		Durin::VulkanRHI::ArmVulkanCreateFailure(
			Durin::VulkanRHI::EVulkanCreateFailurePoint::Sampler);
		const Durin::FByteBuffer FailedResourcePixels =
			Capture(ReloadedMesh, FailedResourceMaterial, false,
				Durin::Editor::EThumbnailCaptureState::Failed);
		EXPECT_TRUE(FailedResourcePixels.empty());
		// Failed views publish neither public statistics nor a private snapshot.
		ASSERT_EQ(TelemetrySnapshots.size(), 7u);
		const std::array<size_t, 7> ExpectedSections{1u, 1u, 1u, 1u, 4u, 1u, 4u};
		for (size_t Index = 0; Index < ExpectedSections.size(); ++Index)
		{
			const Durin::FViewRenderTelemetry& Telemetry =
				TelemetrySnapshots[Index];
			EXPECT_EQ(Telemetry.StaticMesh.VisibleStaticMeshCandidates, 1u);
			EXPECT_EQ(Telemetry.StaticMesh.PreparedStaticMeshPrimitives, 1u);
			EXPECT_EQ(
				Telemetry.StaticMesh.PreparedStaticMeshSections, ExpectedSections[Index]);
			EXPECT_EQ(
				Telemetry.StaticMesh.OpaqueStaticMeshSections, ExpectedSections[Index]);
			EXPECT_EQ(Telemetry.StaticMesh.OpaqueStaticMeshStateGroups, 1u);
			EXPECT_EQ(Telemetry.StaticMesh.StaticMeshResourceAttemptedDraws,
				ExpectedSections[Index]);
			EXPECT_EQ(Telemetry.StaticMesh.StaticMeshResourceSuccessfulDraws,
				ExpectedSections[Index]);
			EXPECT_EQ(Telemetry.StaticMesh.StaticMeshResourceRejectedDraws, 0u);
			EXPECT_EQ(Telemetry.StaticMesh.StaticMeshAttemptedDraws, 0u);
			EXPECT_EQ(Telemetry.StaticMesh.StaticMeshSuccessfulDraws, 0u);
			EXPECT_EQ(Telemetry.StaticMesh.StaticMeshRejectedDraws, 0u);
			EXPECT_EQ(Telemetry.GBuffer.GBufferAttemptedDraws, ExpectedSections[Index]);
			EXPECT_EQ(Telemetry.GBuffer.GBufferSuccessfulDraws, ExpectedSections[Index]);
			EXPECT_EQ(Telemetry.GBuffer.GBufferRejectedDraws, 0u);
			EXPECT_EQ(Telemetry.Deferred.HybridDeferredEnabledViews, 1u);
		}
		// Retain the actual importer output for packed source channels, independent
		// derived textures, transformed UV1, normal strength and masked rendering.
		auto PbrImport = Durin::AssetForge::Builtins::ImportSceneAssets(
			(std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "StaticModelMaterials/ImportedPbrContract.gltf").generic_string(),
			MakeAssetPath("/SceneImportVulkan/Imports/PbrBaseline"),
			Durin::FStaticMeshImportSettings::MakeDurin());
		ASSERT_TRUE(PbrImport) << PbrImport.Message;
		Durin::FPackagePath PbrMeshPath;
		for (const auto& Output : PbrImport.Outputs)
			if (Output.AssetClassName == Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString())
				PbrMeshPath = Output.AssetPath;
		ASSERT_TRUE(PbrMeshPath.IsValid());
		Durin::DStaticMesh* PbrMesh = nullptr;
		{
			auto LoadedValue = Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(PbrMeshPath));
			PbrMesh = LoadedValue.value_or(nullptr);
			ASSERT_TRUE(LoadedValue);
		}
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*PbrMesh);
		const auto* PbrSlot = PbrMesh->GetMaterialSlot(0);
		ASSERT_NE(PbrSlot, nullptr);
		auto* PbrMaterial = Durin::Cast<Durin::DMaterial>(PbrSlot->DefaultMaterial.Get());
		ASSERT_NE(PbrMaterial, nullptr);
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*PbrMaterial);
		const auto PbrProgram = PbrMaterial->GetAcceptedCompiledProgram();
		ASSERT_NE(PbrProgram, nullptr);
		auto* PbrParent = PbrMaterial;
		ASSERT_NE(PbrParent, nullptr);
		const auto& PbrGraph = PbrParent->GetExpressionCollection();
		EXPECT_EQ(std::ranges::count_if(PbrGraph.Expressions,
			[](const auto& Expression) { return Durin::Cast<Durin::DMaterialExpressionTextureSampleParameter2D>(Expression.Get()) != nullptr; }), 6);
		EXPECT_TRUE(std::ranges::none_of(PbrGraph.Expressions,
			[](const auto& Expression) { return Durin::Cast<Durin::DMaterialExpressionFunctionCall>(Expression.Get()) != nullptr; }));
		std::cout << "[SurfaceAcceptance] complex_nodes=" << PbrGraph.Expressions.size()
			<< " owners=" << PbrParent->GetParameterDefinitions().size() << '\n';
		const auto PbrPixels = Capture(PbrMesh, PbrMaterial);
		SaveImportedFunctionBaseline("imported-packed-source-independent-maps-uv1-mask", PbrPixels);
		std::cout << "[FunctionMigrationBaseline] case=imported-packed-source-independent-maps-uv1-mask"
			<< " resources=" << PbrProgram->Layout.ResourceFieldCount
			<< " uniform_fields=" << PbrProgram->Layout.UniformFieldCount
			<< " uniform_bytes=" << PbrProgram->Layout.UniformPayloadSize
			<< " generated_bytes=" << PbrProgram->GeneratedSource.size() << '\n';
		const auto ImportedProgram = ReloadedMaterial->GetAcceptedCompiledProgram();
		ASSERT_NE(ImportedProgram, nullptr);
		std::cout << "[FunctionMigrationBaseline] case=imported-texture-and-factor"
			<< " resources=" << ImportedProgram->Layout.ResourceFieldCount
			<< " uniform_fields=" << ImportedProgram->Layout.UniformFieldCount
			<< " uniform_bytes=" << ImportedProgram->Layout.UniformPayloadSize
			<< " generated_bytes=" << ImportedProgram->GeneratedSource.size() << '\n';
		ASSERT_TRUE(Durin::UnloadPackage(PbrMeshPath));
		for (const auto& Output : PbrImport.Outputs)
			if (Output.AssetClassName == Durin::DMaterial::StaticClass()->GetQualifiedName().ToString())
				ASSERT_TRUE(Durin::UnloadPackage(Output.AssetPath));
		for (const auto& Output : PbrImport.Outputs)
			if (Output.AssetClassName == Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString())
				ASSERT_TRUE(Durin::UnloadPackage(Output.AssetPath));
		ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(PbrMeshPath));
		for (const auto& Output : PbrImport.Outputs)
			if (Output.AssetClassName == Durin::DMaterial::StaticClass()->GetQualifiedName().ToString())
				ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(Output.AssetPath));
		for (const auto& Output : PbrImport.Outputs)
			if (Output.AssetClassName == Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString())
				ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(Output.AssetPath));
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
	EXPECT_TRUE(Sphere->GetRenderData() == nullptr
		|| Sphere->GetRenderData()->GetNumInitializedResources() == 0u);
	Durin::MarkAsGarbage(FactorOnly);
	Durin::MarkAsGarbage(TextureOnly);
	Durin::MarkAsGarbage(FailedResourceMaterial);
	PreloadedSphere = {};
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::UnloadPackage(LODContractPath));
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(MeshPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(MaterialPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(TexturePath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(LODContractPath));
	Durin::FPackagePath StudioPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Engine/Renderer/ThumbnailStudioCube", StudioPath));
	ASSERT_TRUE(Durin::UnloadPackage(StudioPath));
	Durin::CollectGarbage();
	RendererLifecycle.Shutdown();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
	Durin::ShutdownAssetCompilingManager();
	Durin::Testing::RemoveTestWorkDirectory(Root);
}
