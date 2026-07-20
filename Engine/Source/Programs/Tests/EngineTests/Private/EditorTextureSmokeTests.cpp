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
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
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

	TEST(FEditorTextureSmokeTests, ImportsTextureAssignsMaterialAndBuildsVisibleStaticMeshProxy)
	{
		InitializeDObjectSystem();
		static const bool bMountInitialized = [] {
			const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "EditorTextureSmoke";
			std::filesystem::remove_all(Root);
			PathUtilities::RegisterMountPoint("/EditorTextureSmoke/", Root.generic_string() + "/");
			return true;
		}();
		(void)bMountInitialized;

		const std::filesystem::path TextureSource = std::filesystem::path(DURIN_TEST_WORK_DIR) / "EditorTextureSmoke.png";
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
		Material->SetTextureParameterValue(MaterialParameterBaseColorTexture, TextureImport.Asset);

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
		EXPECT_FALSE(LOD.Indices.empty());
		EXPECT_GT(LOD.NumTexCoords, 0u);
		ASSERT_EQ(LOD.TexCoords[0].size(), LOD.Positions.size());
		EXPECT_EQ(StaticMeshProxy->GetMaterialRenderData().BaseColorTexture, TextureImport.Asset->GetRenderResource());

		PrimitiveProxy.reset();
		MarkObjectHierarchyAsGarbage(Actor);
		CollectGarbage();
		FAssetPath MeshPath;
		FAssetPath TexturePath;
		ASSERT_TRUE(FAssetPath::TryCreate("/EditorTextureSmoke/Meshes/VisibleMesh", MeshPath));
		ASSERT_TRUE(FAssetPath::TryCreate("/EditorTextureSmoke/Textures/BaseColor", TexturePath));
		ASSERT_TRUE(Asset::UnloadPackage(MaterialPath));
		ASSERT_TRUE(Asset::UnloadPackage(MeshPath));
		ASSERT_TRUE(Asset::UnloadPackage(TexturePath));
	}
} // namespace Durin
