#include "AssetSystem.h"
#include "Asset/EditorAssetRetention.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/PrimitiveSceneProxy.h"
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
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMeshTestAccess.h"
#include "StaticModelImportBuild.h"
#include "Thumbnail/AssetThumbnail.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Texture/Texture2D.h"
#include "TextureTestSupport.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>

namespace
{
	class FStaticModelImportRenderEngine final : public Durin::DEngine
	{
	public:
		FStaticModelImportRenderEngine()
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

TEST(FStaticModelImportVulkanTests, RendersReloadedSrgbTextureAndBaseColorFactor)
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
		Durin::Testing::GetTestWorkDirectory() / "StaticModelImportVulkan";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FScopedDerivedDataCacheRoot DerivedDataCache(Root / "DerivedDataCache");
	for (const std::filesystem::path& Directory : {
		Root / "Engine/Content",
		Root / "Engine/SourceAssets",
		Root / "Project/Content",
		Root / "Project/SourceAssets"})
	{
		std::filesystem::create_directories(Directory);
	}
	const std::array Mounts{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Engine",
			.ContentRoot = Root / "Engine/Content",
			.SourceAssetsRoot = Root / "Engine/SourceAssets",
			.bSourceWritable = true},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/StaticModelImportVulkan/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.OwnerRoot = Root / "Project",
			.ContentRoot = Root / "Project/Content",
			.SourceAssetsRoot = Root / "Project/SourceAssets",
			.bSourceWritable = true,
			.Dependencies = {"/Engine/"}}};
	Durin::PathUtilities::FScopedMountRegistryFixture MountFixture(Mounts);
	ASSERT_TRUE(MountFixture.IsValid()) << MountFixture.GetError();

	const Durin::FAssetPath RootPath =
		MakeAssetPath("/StaticModelImportVulkan/Imports/RenderedOpaque");
	const Durin::FStaticModelImportPlanResult Planned =
		Durin::PlanStaticModelImport({
			.SourceFile = std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "StaticModelMaterials/RenderedOpaqueDataUri.gltf",
			.RootAssetPath = RootPath,
			.RootSourceDestination = {
				.Path = "/StaticModelImportVulkan/Models/RenderedOpaqueDataUri.gltf"}});
	ASSERT_TRUE(Planned) << Planned.Message;
	ASSERT_EQ(Planned.Plan.Assets.size(), 3u);
	const Durin::FAssetPath TexturePath = Planned.Plan.Assets[1].AssetPath;
	const Durin::FAssetPath MaterialPath = Planned.Plan.Assets[2].AssetPath;
	const Durin::FAssetPath StandardPath =
		MakeAssetPath(Durin::StandardImportedSurfaceMaterialPath);
	const Durin::FStaticModelImportExecutionResult Executed =
		Durin::ExecuteStaticModelImport(Planned.Plan);
	ASSERT_TRUE(Executed) << Executed.Message;
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));

	const Durin::FAssetPath LODContractPath =
		MakeAssetPath("/StaticModelImportVulkan/LODContract");
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
	LifecycleMesh->InitResources();
	LifecycleMesh->InitResources();
	Durin::FlushRenderingCommands();
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
	ASSERT_EQ(LifecycleVertexFactory.GetStreams().size(), 2u);
	EXPECT_EQ(
		LifecycleVertexFactory.GetStreams()[0].VertexBuffer,
		LifecycleLOD.VertexBuffers.PositionVertexBuffer.GetRHI());
	EXPECT_EQ(
		LifecycleVertexFactory.GetStreams()[1].VertexBuffer,
		LifecycleLOD.VertexBuffers.StaticMeshVertexBuffer.GetRHI());
	EXPECT_TRUE(std::ranges::none_of(
		LifecycleVertexFactory.GetStreams(),
		[&LifecycleLOD](const Durin::FVertexInputStream& Stream) {
			return Stream.VertexBuffer == LifecycleLOD.IndexBuffer.GetRHI();
		}));
	EXPECT_EQ(
		LifecycleMesh->GetRenderData()->GetNumInitializedResources(),
		7u);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 7u);

	const Durin::FStaticMeshRenderData* OriginalRenderData =
		LifecycleMesh->GetRenderData();
	Durin::DStaticMesh* ReplacementCandidate =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::AddToRoot(ReplacementCandidate);
	const Durin::FStaticMeshRenderData* ReplacementRenderData =
		ReplacementCandidate->GetRenderData();
	std::string ReplacementError;
	ASSERT_TRUE(LifecycleMesh->ExchangeImportedState(
		*ReplacementCandidate, ReplacementError))
		<< ReplacementError;
	EXPECT_EQ(LifecycleMesh->GetRenderData(), ReplacementRenderData);
	EXPECT_EQ(ReplacementCandidate->GetRenderData(), OriginalRenderData);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 7u);

	ASSERT_TRUE(LifecycleMesh->ExchangeImportedState(
		*ReplacementCandidate, ReplacementError))
		<< ReplacementError;
	EXPECT_EQ(LifecycleMesh->GetRenderData(), OriginalRenderData);
	EXPECT_EQ(
		ReplacementCandidate->GetRenderData(),
		ReplacementRenderData);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 7u);

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
		->LODResources[1].Sections[0].MaterialSlotIndex = 99;
	EXPECT_FALSE(LifecycleMesh->ExchangeImportedState(
		*FailedReplacementCandidate, ReplacementError));
	EXPECT_EQ(LifecycleMesh->GetRenderData(), OriginalRenderData);
	EXPECT_EQ(
		FailedReplacementCandidate->GetRenderData()
			->GetNumInitializedResources(),
		0u);
	EXPECT_EQ(
		Durin::GetNumInitializedRenderResources(),
		InitialRenderResourceCount + 7u);

	Durin::DStaticMesh* InvalidMesh =
		Durin::DStaticMesh::CreateDebugTriangle();
	Durin::AddToRoot(InvalidMesh);
	Durin::FStaticMeshTestAccess::GetMutableRenderData(InvalidMesh)
		->LODResources.push_back(
		InvalidMesh->GetRenderData()->LODResources[0]);
	Durin::FStaticMeshTestAccess::GetMutableRenderData(InvalidMesh)
		->LODResources[1].Sections[0].MaterialSlotIndex = 99;
	InvalidMesh->InitResources();
	Durin::FlushRenderingCommands();
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
	Durin::RemoveFromRoot(LifecycleMesh);
	Durin::MarkAsGarbage(LifecycleMesh);
	Durin::CollectGarbage();
	EXPECT_NE(Durin::ResolveObjectHandle(LifecycleHandle), nullptr);
	EXPECT_FALSE(LifecycleMesh->IsReadyForFinishDestroy());
	{
		std::lock_guard Lock(BlockedRenderCommand->Mutex);
		BlockedRenderCommand->bContinue = true;
	}
	BlockedRenderCommand->CV.notify_all();
	Durin::FlushRenderingCommands();
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
	ASSERT_TRUE(Durin::Asset::LoadAsset(RootPath, ReloadedMesh));
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

	struct FBeginStaticModelImportFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginStaticModelImportFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginStaticModelImportFrame>(
		[](Durin::FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Durin::GDynamicRHI->RHIBeginFrame();
		});

	FStaticModelImportRenderEngine Engine;
	Durin::FRendererModule Renderer;
	Renderer.StartupModule();
	Engine.SetRenderer(&Renderer);
	Durin::GEngine = &Engine;
	Durin::DMaterialInstance* TextureOnly =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "TextureOnlyControl");
	Durin::DMaterialInstance* FactorOnly =
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "FactorOnlyControl");
	ASSERT_TRUE(TextureOnly->SetParent(ReloadedMaterial->GetParent()));
	ASSERT_TRUE(TextureOnly->SetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ReloadedTexture));
	ASSERT_TRUE(TextureOnly->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(1.0, 1.0, 1.0)));
	ASSERT_TRUE(FactorOnly->SetParent(ReloadedMaterial->GetParent()));
	ASSERT_TRUE(FactorOnly->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), ImportedFactor));
	Durin::FlushRenderingCommands();

	Durin::FRenderedAssetThumbnailVisualContract Contract;
	Contract.Output.Width = 64;
	Contract.Output.Height = 64;
	{
		Durin::FRenderedAssetThumbnailPreviewScenePool Pool(Contract);
		ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
		auto Capture = [&](
			Durin::DStaticMesh* Mesh,
			Durin::DMaterialInterface* Material) {
			std::vector<Durin::uint8> Pixels;
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
		const std::vector<Durin::uint8> LODContractPixels =
			Capture(LODContractMesh, ReloadedMaterial);
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
			LODContractPixels.size(),
			ImportedPixels.size());
		EXPECT_EQ(
			Durin::FXxHash128::HashBuffer(
				LODContractPixels).ToString(),
			"52fdb5113401075fabb77a111012afd1");

		struct FEndStaticModelImportFrame
		{
			static constexpr auto GetName() -> const char* { return "EndStaticModelImportFrame"; }
		};
		Durin::EnqueueRenderCommand<FEndStaticModelImportFrame>(
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
	PreloadedSphere = {};
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LODContractPath));
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
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
