#include <gtest/gtest.h>

#include "EngineTestSupport.h"

#include "Actors/StaticMeshActor.h"
#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialTypes.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StandardAssetImportProviders.h"
#include "Texture/Texture2D.h"

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
		MaterialSnapshotSurvivesTextureReplacementProxyClosureAndAssetUnload)
	{
		InitializeDObjectSystem();
		std::string ProviderError;
		ASSERT_TRUE(RegisterStandardAssetImportProviders(ProviderError)) << ProviderError;
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
		const FTexture2DImportResult TextureImport = DTexture2D::ImportAsset(TextureSource.generic_string(), "/EditorTextureSmoke/Textures/BaseColor");
		ASSERT_TRUE(TextureImport) << TextureImport.Message;
		ASSERT_NE(TextureImport.Asset, nullptr);
		ASSERT_NE(TextureImport.Asset->GetSourceData(), nullptr);
		EXPECT_EQ(TextureImport.Asset->GetSourceData()->Pixels.size(), 8u);

		const std::filesystem::path MeshSource = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		const FStaticMeshImportResult MeshImport = DStaticMesh::ImportAsset(MeshSource.generic_string(), "/EditorTextureSmoke/Meshes/VisibleMesh");
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

		std::unique_ptr<PrimitiveSceneProxy> PrimitiveProxy = Component->CreateSceneProxy();
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
					FMaterialRenderV3Binding Binding;
					FMaterialRenderValidationDiagnostic Diagnostic;
					const bool bValid = TryGetMaterialRenderV3Binding(
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
		ASSERT_TRUE(TextureImport.Asset->SetSRGB(
			!TextureImport.Asset->IsSRGB(), RebuildError)) << RebuildError;
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
		MarkObjectHierarchyAsGarbage(Actor);
		CollectGarbage();
		const Asset::FAssetResult MaterialUnload =
			Asset::UnloadPackage(MaterialPath);
		const Asset::FAssetResult MeshUnload =
			Asset::UnloadPackage(MeshPath);
		const Asset::FAssetResult TextureUnload =
			Asset::UnloadPackage(TexturePath);
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
		const FTexture2DImportResult Import = DTexture2D::ImportAsset(
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
