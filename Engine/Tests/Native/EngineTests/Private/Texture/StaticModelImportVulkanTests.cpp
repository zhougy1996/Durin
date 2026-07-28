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
#include "StaticModelImportBuild.h"
#include "Thumbnail/AssetThumbnail.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Texture/Texture2D.h"
#include "TextureTestSupport.h"

#include <gtest/gtest.h>

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
	ASSERT_FALSE(LODContract.Positions.empty());
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.Normals,
		[](const Durin::FVector3f& Normal) {
			return std::abs(Normal.y) > 0.1f
				&& std::abs(Normal.z) < 0.99f;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.Tangents,
		[](const Durin::FVector4f& Tangent) {
			return Tangent.w < 0.0f;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.TexCoords[0],
		[](const Durin::FVector2f& UV) {
			return UV.x != 0.0f || UV.y != 0.0f;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		LODContract.Colors,
		[](const Durin::FVector4f& Color) {
			return Color != Durin::FVector4f(1.0f);
		}));

	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();

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
			Durin::DMaterialInterface* Material,
			Durin::uint64 Revision) {
			std::vector<Durin::uint8> Pixels;
			std::unique_ptr<Durin::PrimitiveSceneProxy> Proxy =
				Durin::CreateMaterialPreviewPrimitive(
					Mesh, Material, Revision, Error);
			EXPECT_NE(Proxy, nullptr) << Error;
			if (Proxy == nullptr) return Pixels;
			EXPECT_TRUE(Pool.SetPrimitive(
				std::move(Proxy), Durin::FMatrix(1.0), Error)) << Error;
			EXPECT_TRUE(Pool.BeginCapture(Error, false)) << Error;
			Durin::FlushRenderingCommands();
			EXPECT_EQ(
				Pool.PollCapture(Pixels, Error),
				Durin::ERenderedAssetThumbnailCaptureState::Ready) << Error;
			Pool.Reset();
			return Pixels;
		};

		const std::vector<Durin::uint8> ImportedPixels =
			Capture(ReloadedMesh, ReloadedMaterial, 1);
		const std::vector<Durin::uint8> TextureOnlyPixels =
			Capture(ReloadedMesh, TextureOnly, 2);
		const std::vector<Durin::uint8> FactorOnlyPixels =
			Capture(ReloadedMesh, FactorOnly, 3);
		const std::vector<Durin::uint8> LODContractPixels =
			Capture(LODContractMesh, ReloadedMaterial, 4);
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
	ReloadedMesh->GetRenderData()->ReleaseResources();
	LODContractMesh->GetRenderData()->ReleaseResources();
	auto* Sphere = Durin::Cast<Durin::DStaticMesh>(PreloadedSphere.Get());
	ASSERT_NE(Sphere, nullptr);
	Sphere->GetRenderData()->ReleaseResources();
	Renderer.ReleaseResources();
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(FactorOnly);
	Durin::MarkAsGarbage(TextureOnly);
	PreloadedSphere = {};
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RootPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MaterialPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(TexturePath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StandardPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LODContractPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RootPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MaterialPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(TexturePath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(StandardPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(LODContractPath));
	Renderer.ShutdownModule();
	Durin::ShutdownRenderingThread();
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
	Durin::Testing::RemoveTestWorkDirectory(Root);
}
