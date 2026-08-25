#include <gtest/gtest.h>

#include "EngineTestSupport.h"

#include "Actors/StaticMeshActor.h"
#include "Asset/PackageVersionPolicy.h"
#include "AssetTools.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "Editor/WorkspaceManager.h"
#include "MaterialEditorModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "AssetForgeBuiltinsProviders.h"
#include "AssetForgeBuiltinsAuthoringTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"

namespace Durin
{
	namespace
	{
		auto WriteTextureSmokeFixture(const std::filesystem::path& Path) -> void
		{
			// 2x1 RGBA PNG with opaque red next to transparent black, so a white fallback is distinguishable in visual follow-up runs.
			constexpr uint8 PngBytes[] = {
				137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
				0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
				0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream.write(reinterpret_cast<const char*>(PngBytes), static_cast<std::streamsize>(std::size(PngBytes)));
		}

	}

	TEST(FEditorTextureSmokeTests,
		OrdinaryGraphRendersReloadsAndResavesDeterministically)
	{
		InitializeDObjectSystem();
		ASSERT_TRUE(Tests::InstallAssetForgeBuiltinsAuthoringFeatures());
		std::string ProviderError;
		ASSERT_TRUE(AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
			ProviderError, GetEngineTestModuleCallbackGate()))
			<< ProviderError;
		InitRenderingThread();
		const std::filesystem::path Root =
			Testing::GetTestWorkDirectory() / "EditorMixedV4Rendering";
		Durin::Testing::RemoveTestWorkDirectory(Root);
		std::filesystem::create_directories(Root / "Content");
		const std::array Definitions{
			PathUtilities::FMountPoint{
				.VirtualRoot = "/EditorMixedV4/",
				.Owner = PathUtilities::EMountOwner::Test,
				.Root = Root / "Content",
				.ContentPath = ".",
				.bAutoScan = true,
				.bAuthoringWritable = true}};
		PathUtilities::FScopedMountRegistryFixture Mounts(Definitions);
		ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
		FPaths::SetDerivedDataCacheDirForTests(
			(Root / "DerivedDataCache").generic_string());

		const std::filesystem::path TextureSource =
			Root / "VisibleTexture.png";
		WriteTextureSmokeFixture(TextureSource);
		const FTexture2DImportResult TextureImport = AssetForge::Builtins::ImportTexture2DAsset(
			TextureSource.generic_string(), "/EditorMixedV4/Textures/BaseColor");
		ASSERT_TRUE(TextureImport) << TextureImport.Message;
		const std::filesystem::path MeshSource =
			std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		const FStaticMeshImportResult MeshImport = AssetForge::Builtins::ImportStaticMeshAsset(
			MeshSource.generic_string(), "/EditorMixedV4/Meshes/VisibleMesh");
		ASSERT_TRUE(MeshImport) << MeshImport.Message;
		FAssetPath MaterialPath;
		FAssetPath MeshPath;
		FAssetPath TexturePath;
		ASSERT_TRUE(FAssetPath::TryCreate(
			"/EditorMixedV4/Materials/Textured", MaterialPath));
		ASSERT_TRUE(FAssetPath::TryCreate(
			"/EditorMixedV4/Meshes/VisibleMesh", MeshPath));
		ASSERT_TRUE(FAssetPath::TryCreate(
			"/EditorMixedV4/Textures/BaseColor", TexturePath));
		DMaterial* Material = nullptr;
		ASSERT_TRUE(Asset::CreateAsset(MaterialPath, Material));
		Material->SetTextureParameterValue(
			MaterialParameters::BaseColorTextureName(), TextureImport.Asset);
		ASSERT_TRUE(Asset::SavePackage(Material->GetPackage()));

		ASSERT_TRUE(Asset::UnloadPackage(MaterialPath));
		ASSERT_TRUE(Asset::UnloadPackage(MeshPath));
		ASSERT_TRUE(Asset::UnloadPackage(TexturePath));
		for (const FAssetPath& Path : {MaterialPath, MeshPath, TexturePath})
		{
			const Asset::FAssetCatalogEntry Data =
				Asset::FindAssetExact(Path);
			ASSERT_NE(Data, nullptr);
			EXPECT_EQ(Data->FormatVersion,
				Asset::OrdinaryAssetPackageWriterVersion);
		}

		auto LoadRenderableGraph = [&]() {
			DStaticMesh* Mesh = nullptr;
			DMaterial* LoadedMaterial = nullptr;
			DTexture2D* Texture = nullptr;
			Asset::FAssetLoadReport MeshReport;
			Asset::FAssetLoadReport MaterialReport;
			Asset::FAssetLoadReport TextureReport;
			EXPECT_TRUE(Asset::LoadAsset(MeshPath, Mesh, &MeshReport));
			EXPECT_TRUE(Asset::LoadAsset(MaterialPath, LoadedMaterial,
				&MaterialReport));
			EXPECT_TRUE(Asset::LoadAsset(TexturePath, Texture, &TextureReport));
			return std::tuple{Mesh, LoadedMaterial, Texture};
		};
		auto ValidateRenderableGraph = [](DStaticMesh* Mesh,
			DMaterial* LoadedMaterial, DTexture2D* Texture) {
			AStaticMeshActor* Actor = NewObject<AStaticMeshActor>(
				nullptr, "MixedV4RenderActor");
			DStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(Mesh);
			Component->SetMaterial(LoadedMaterial);
			std::unique_ptr<FPrimitiveSceneProxy> Proxy =
				Component->CreateSceneProxy();
			auto* StaticMeshProxy =
				dynamic_cast<FStaticMeshSceneProxy*>(Proxy.get());
			EXPECT_NE(StaticMeshProxy, nullptr);
			EXPECT_NE(StaticMeshProxy->GetRenderData(), nullptr);
			FRHITextureReferenceRef BoundTexture;
			struct FCaptureMixedV4Texture
			{
				static constexpr auto GetName() -> const char*
				{
					return "CaptureMixedV4Texture";
				}
			};
			EnqueueRenderCommand<FCaptureMixedV4Texture>(
				[StaticMeshProxy, &BoundTexture](FRHICommandListImmediate&) {
					FMaterialRenderBinding Binding;
					FMaterialRenderValidationDiagnostic Diagnostic;
					EXPECT_TRUE(TryGetMaterialRenderBinding(
						StaticMeshProxy->ResolveMaterialRenderData_RenderThread()
							.Representation,
						Binding, Diagnostic)) << Diagnostic.Message;
					BoundTexture = Binding.Textures[0];
				});
			FlushRenderingCommands();
			EXPECT_EQ(BoundTexture, Texture->GetTextureReferenceRHI());
			Proxy.reset();
			MarkObjectHierarchyAsGarbage(Actor);
			CollectGarbage();
		};

		auto [LoadedMesh, LoadedMaterial, LoadedTexture] =
			LoadRenderableGraph();
		ASSERT_NE(LoadedMesh, nullptr);
		ASSERT_NE(LoadedMaterial, nullptr);
		ASSERT_NE(LoadedTexture, nullptr);
		ValidateRenderableGraph(LoadedMesh, LoadedMaterial, LoadedTexture);
		ASSERT_TRUE(Asset::UnloadPackage(MaterialPath));
		ASSERT_TRUE(Asset::UnloadPackage(MeshPath));
		ASSERT_TRUE(Asset::UnloadPackage(TexturePath));

		std::tie(LoadedMesh, LoadedMaterial, LoadedTexture) =
			LoadRenderableGraph();
		ASSERT_NE(LoadedMesh, nullptr);
		ASSERT_NE(LoadedMaterial, nullptr);
		ASSERT_NE(LoadedTexture, nullptr);
		ValidateRenderableGraph(LoadedMesh, LoadedMaterial, LoadedTexture);
		const std::string MaterialFile =
			Asset::FindAssetExact(MaterialPath)->PhysicalPath;
		std::vector<std::byte> BeforeSave;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(
			BeforeSave, MaterialFile));
		Editor::FWorkspaceManager WorkspaceManager;
		Editor::FRenderedAssetThumbnailService ThumbnailService;
		Durin::FMaterialEditorModule MaterialEditorModule;
		ASSERT_TRUE(MaterialEditorModule.RegisterMaterialEditor(
			WorkspaceManager, ThumbnailService));
		ASSERT_TRUE(WorkspaceManager.OpenAsset(
			MaterialPath.ToString(),
			DMaterial::StaticClass()->GetQualifiedName().ToString()));
		auto MaterialWorkspace = WorkspaceManager.FindWorkspace(
			Editor::FWorkspaceTypeId("MaterialEditor"));
		ASSERT_NE(MaterialWorkspace, nullptr);
		LoadedMaterial->MarkPackageDirty();
		EXPECT_TRUE(MaterialWorkspace->CanSaveActiveDocument());
		EXPECT_TRUE(MaterialWorkspace->SaveActiveDocument());
		EXPECT_FALSE(LoadedMaterial->GetPackage()->IsDirty());
		std::vector<std::byte> AfterSave;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(
			AfterSave, MaterialFile));
		EXPECT_EQ(AfterSave, BeforeSave);
		EXPECT_EQ(Asset::FindAssetExact(MaterialPath)
			->FormatVersion, Asset::OrdinaryAssetPackageWriterVersion);

		ShutdownRenderingThread();
		EXPECT_EQ(GetNumPendingRenderCommands(), 0u);
	}

	TEST(FEditorTextureSmokeTests,
		MaterialSnapshotSurvivesTextureReplacementProxyClosureAndAssetUnload)
	{
		InitializeDObjectSystem();
		ASSERT_TRUE(Tests::InstallAssetForgeBuiltinsAuthoringFeatures());
		std::string ProviderError;
		ASSERT_TRUE(AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
			ProviderError, GetEngineTestModuleCallbackGate())) << ProviderError;
		ASSERT_TRUE(AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
			ProviderError, GetEngineTestModuleCallbackGate())) << ProviderError;
		EXPECT_TRUE(ProviderError.empty());
		InitRenderingThread();
		const std::filesystem::path Root = Testing::GetTestWorkDirectory() / "EditorTextureSmoke";
		static std::unordered_set<std::filesystem::path> InitializedRoots;
		if (InitializedRoots.insert(Root).second)
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			PathUtilities::RegisterMountPointForTests("/EditorTextureSmoke/", Root.generic_string() + "/");
		}

		const std::filesystem::path TextureSource = Testing::GetTestWorkDirectory() / "EditorTextureSmoke.png";
		WriteTextureSmokeFixture(TextureSource);
		const FTexture2DImportResult TextureImport = AssetForge::Builtins::ImportTexture2DAsset(TextureSource.generic_string(), "/EditorTextureSmoke/Textures/BaseColor");
		ASSERT_TRUE(TextureImport) << TextureImport.Message;
		ASSERT_NE(TextureImport.Asset, nullptr);
		ASSERT_NE(TextureImport.Asset->GetSourceData(), nullptr);
		EXPECT_EQ(TextureImport.Asset->GetSourceData()->Pixels.size(), 8u);

		const std::filesystem::path MeshSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		const FStaticMeshImportResult MeshImport = AssetForge::Builtins::ImportStaticMeshAsset(MeshSource.generic_string(), "/EditorTextureSmoke/Meshes/VisibleMesh");
		ASSERT_TRUE(MeshImport) << MeshImport.Message;
		ASSERT_NE(MeshImport.Asset, nullptr);

		FAssetPath MaterialPath;
		ASSERT_TRUE(FAssetPath::TryCreate("/EditorTextureSmoke/Materials/Textured", MaterialPath));
		DMaterial* Material = nullptr;
		ASSERT_TRUE(Asset::CreateAsset(MaterialPath, Material));
		Material->SetTextureParameterValue(MaterialParameters::BaseColorTextureName(), TextureImport.Asset);

		AStaticMeshActor* Actor = NewObject<AStaticMeshActor>(nullptr, "TextureSmokeMesh");
		ASSERT_NE(Actor, nullptr);
		DStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		ASSERT_NE(Component, nullptr);
		Component->SetStaticMesh(MeshImport.Asset);
		Component->SetMaterial(Material);

		std::unique_ptr<FPrimitiveSceneProxy> PrimitiveProxy = Component->CreateSceneProxy();
		auto* StaticMeshProxy = dynamic_cast<FStaticMeshSceneProxy*>(PrimitiveProxy.get());
		ASSERT_NE(StaticMeshProxy, nullptr);
		ASSERT_NE(StaticMeshProxy->GetRenderData(), nullptr);
		ASSERT_FALSE(StaticMeshProxy->GetRenderData()->LODResources.empty());
		const FStaticMeshLODResources& LOD = StaticMeshProxy->GetRenderData()->LODResources[0];
		EXPECT_FALSE(LOD.IndexBuffer.GetIndices().empty());
		EXPECT_GT(LOD.NumTexCoords, 0u);
		ASSERT_EQ(
			LOD.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer.GetTexCoords()[0].size(),
			LOD.GetNumVertices());
		auto CaptureMaterialTextureReference = [StaticMeshProxy]() {
			FRHITextureReferenceRef Result;
			struct FCaptureEditorTextureMaterialReference
			{
				static constexpr auto GetName() -> const char*
				{
					return "CaptureEditorTextureMaterialReference";
				}
			};
			EnqueueRenderCommand<FCaptureEditorTextureMaterialReference>(
				[StaticMeshProxy, &Result](FRHICommandListImmediate&) {
					FMaterialRenderBinding Binding;
					FMaterialRenderValidationDiagnostic Diagnostic;
					const bool bValid = TryGetMaterialRenderBinding(
						StaticMeshProxy
							->ResolveMaterialRenderData_RenderThread()
							.Representation,
						Binding,
						Diagnostic);
					EXPECT_TRUE(bValid) << Diagnostic.Message;
					Result = Binding.Textures[0];
				});
			FlushRenderingCommands();
			return Result;
		};
		EXPECT_EQ(
			CaptureMaterialTextureReference(),
			TextureImport.Asset->GetTextureReferenceRHI());

		FRHITextureReference* StableTextureReference =
			TextureImport.Asset->GetTextureReferenceRHI().GetReference();
		ASSERT_NE(StableTextureReference, nullptr);
		std::string RebuildError;
		ASSERT_TRUE(AssetForge::Builtins::SetTexture2DSRGB(
			*TextureImport.Asset, !TextureImport.Asset->IsSRGB(), RebuildError)) << RebuildError;
		ASSERT_TRUE(Asset::Build::WaitForTexture2DBuild(*TextureImport.Asset, 10.0))
			<< Asset::Build::GetTexture2DBuildDiagnostic(*TextureImport.Asset).Message;
		FlushRenderingCommands();
		EXPECT_EQ(
			TextureImport.Asset->GetTextureReferenceRHI().GetReference(),
			StableTextureReference);
		EXPECT_EQ(
			CaptureMaterialTextureReference().GetReference(),
			StableTextureReference);
		FAssetPath MeshPath;
		FAssetPath TexturePath;
		ASSERT_TRUE(FAssetPath::TryCreate(
			"/EditorTextureSmoke/Meshes/VisibleMesh", MeshPath));
		ASSERT_TRUE(FAssetPath::TryCreate(
			"/EditorTextureSmoke/Textures/BaseColor", TexturePath));

		auto CommandStarted = std::make_shared<std::promise<void>>();
		std::future<void> CommandStartedFuture = CommandStarted->get_future();
		auto AllowCommandCompletion = std::make_shared<std::promise<void>>();
		std::shared_future<void> AllowCommandCompletionFuture =
			AllowCommandCompletion->get_future().share();
		auto bAcceptedReferenceObserved =
			std::make_shared<std::atomic<bool>>(false);
		FRHITextureReferenceRef AcceptedMaterialReference =
			CaptureMaterialTextureReference();
		struct FObserveAcceptedMaterialTextureReference
		{
			static constexpr auto GetName() -> const char*
			{
				return "ObserveAcceptedMaterialTextureReference";
			}
		};
		EnqueueRenderCommand<FObserveAcceptedMaterialTextureReference>(
			[Reference = std::move(AcceptedMaterialReference),
			 CommandStarted,
			 AllowCommandCompletionFuture,
			 bAcceptedReferenceObserved,
			 StableTextureReference](FRHICommandListImmediate&) {
				CommandStarted->set_value();
				AllowCommandCompletionFuture.wait();
				bAcceptedReferenceObserved->store(
					Reference.GetReference() == StableTextureReference,
					std::memory_order_release);
			});
		CommandStartedFuture.wait();
		PrimitiveProxy.reset();
		const Asset::FAssetResult MaterialUnload =
			Asset::UnloadPackage(Material->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		MarkObjectHierarchyAsGarbage(Actor);
		CollectGarbage();
		const Asset::FAssetResult MeshUnload =
			Asset::UnloadPackage(MeshPath);
		const Asset::FAssetResult TextureUnload =
			Asset::UnloadPackage(
				TexturePath,
				Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		AllowCommandCompletion->set_value();
		FlushRenderingCommands();
		EXPECT_TRUE(MaterialUnload) << MaterialUnload.Message;
		EXPECT_TRUE(MeshUnload) << MeshUnload.Message;
		EXPECT_TRUE(TextureUnload) << TextureUnload.Message;
		EXPECT_TRUE(
			bAcceptedReferenceObserved->load(std::memory_order_acquire));
		ShutdownRenderingThread();
		EXPECT_EQ(GetRenderCommandAdmissionState(),
			ERenderCommandAdmissionState::Stopped);
		EXPECT_EQ(GetNumPendingRenderCommands(), 0u);
	}

	TEST(FEditorTextureSmokeTests,
		TextureUnloadBehindQueuedCommandReturnsResourceCountsToBaseline)
	{
		InitializeDObjectSystem();
		InitRenderingThread();
		const size_t InitialRenderResourceCount =
			GetNumInitializedRenderResources();
		const size_t InitialDeferredCleanupCount =
			GetNumPendingRenderResourceCleanup();

		const std::filesystem::path Root =
			Testing::GetTestWorkDirectory() / "TextureOwnershipSmoke";
		Durin::Testing::RemoveTestWorkDirectory(Root);
		PathUtilities::RegisterMountPointForTests(
			"/TextureOwnershipSmoke/", Root.generic_string() + "/");
		const std::filesystem::path Source =
			Testing::GetTestWorkDirectory() / "TextureOwnershipSmoke.png";
		WriteTextureSmokeFixture(Source);
		const FTexture2DImportResult Import = AssetForge::Builtins::ImportTexture2DAsset(
			Source.generic_string(), "/TextureOwnershipSmoke/Texture");
		ASSERT_TRUE(Import) << Import.Message;
		ASSERT_NE(Import.Asset, nullptr);
		FlushRenderingCommands();
		FRHITextureReferenceRef AcceptedReference =
			Import.Asset->GetTextureReferenceRHI();
		ASSERT_NE(AcceptedReference, nullptr);

		auto CommandStarted = std::make_shared<std::promise<void>>();
		std::future<void> CommandStartedFuture = CommandStarted->get_future();
		auto AllowCommandCompletion = std::make_shared<std::promise<void>>();
		std::shared_future<void> AllowCommandCompletionFuture =
			AllowCommandCompletion->get_future().share();
		auto bReferenceObserved = std::make_shared<std::atomic<bool>>(false);
		struct FObserveTextureReferenceDuringUnload
		{
			static constexpr auto GetName() -> const char*
			{
				return "ObserveTextureReferenceDuringUnload";
			}
		};
		EnqueueRenderCommand<FObserveTextureReferenceDuringUnload>(
			[AcceptedReference, CommandStarted, AllowCommandCompletionFuture,
			 bReferenceObserved](FRHICommandListImmediate&) {
				CommandStarted->set_value();
				AllowCommandCompletionFuture.wait();
				bReferenceObserved->store(
					AcceptedReference != nullptr, std::memory_order_release);
			});
		CommandStartedFuture.wait();

		FAssetPath TexturePath;
		ASSERT_TRUE(FAssetPath::TryCreate(
			"/TextureOwnershipSmoke/Texture", TexturePath));
		const Asset::FAssetResult Unload = Asset::UnloadPackage(TexturePath);
		AllowCommandCompletion->set_value();
		FlushRenderingCommands();
		EXPECT_TRUE(Unload) << Unload.Message;
		EXPECT_TRUE(
			bReferenceObserved->load(std::memory_order_acquire));
		EXPECT_EQ(
			GetNumInitializedRenderResources(), InitialRenderResourceCount);
		EXPECT_EQ(
			GetNumPendingRenderResourceCleanup(), InitialDeferredCleanupCount);

		ShutdownRenderingThread();
		EXPECT_EQ(GetNumPendingRenderCommands(), 0u);
	}
} // namespace Durin
#include "TextureAuthoringTestEnvironment.h"
