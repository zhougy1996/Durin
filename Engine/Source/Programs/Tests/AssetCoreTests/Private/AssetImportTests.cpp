#include <gtest/gtest.h>

#include "AssetCore.h"
#include "Threading/QueuedThreadPool.h"

namespace Durin::Asset
{
	namespace
	{
		class FEngineThreadPoolTestGuard
		{
		public:
			~FEngineThreadPoolTestGuard()
			{
				ShutdownEngineThreadPool(false);
			}
		};

		auto ExpectVec3Eq(const glm::vec3& Expected, const glm::vec3& Actual) -> void
		{
			EXPECT_FLOAT_EQ(Expected.x, Actual.x);
			EXPECT_FLOAT_EQ(Expected.y, Actual.y);
			EXPECT_FLOAT_EQ(Expected.z, Actual.z);
		}

		auto ExpectVec2Eq(const glm::vec2& Expected, const glm::vec2& Actual) -> void
		{
			EXPECT_FLOAT_EQ(Expected.x, Actual.x);
			EXPECT_FLOAT_EQ(Expected.y, Actual.y);
		}

		auto ExpectVec4Eq(const glm::vec4& Expected, const glm::vec4& Actual) -> void
		{
			EXPECT_FLOAT_EQ(Expected.x, Actual.x);
			EXPECT_FLOAT_EQ(Expected.y, Actual.y);
			EXPECT_FLOAT_EQ(Expected.z, Actual.z);
			EXPECT_FLOAT_EQ(Expected.w, Actual.w);
		}

		auto ExpectTriangleMesh(const FImportedMeshData& Mesh) -> void
		{
			ASSERT_EQ(3u, Mesh.Positions.size());
			ASSERT_EQ(3u, Mesh.Normals.size());
			ASSERT_EQ(3u, Mesh.Tangents.size());
			ASSERT_EQ(3u, Mesh.UVChannels[0].size());
			ASSERT_EQ(3u, Mesh.Indices.size());

			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 0.0f), Mesh.Positions[0]);
			ExpectVec3Eq(glm::vec3(1.0f, 0.0f, 0.0f), Mesh.Positions[1]);
			ExpectVec3Eq(glm::vec3(0.0f, 1.0f, 0.0f), Mesh.Positions[2]);

			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 1.0f), Mesh.Normals[0]);
			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 1.0f), Mesh.Normals[1]);
			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 1.0f), Mesh.Normals[2]);

			ExpectVec2Eq(glm::vec2(0.0f, 1.0f), Mesh.UVChannels[0][0]);
			ExpectVec2Eq(glm::vec2(1.0f, 1.0f), Mesh.UVChannels[0][1]);
			ExpectVec2Eq(glm::vec2(0.0f, 0.0f), Mesh.UVChannels[0][2]);
			for (const glm::vec4& Tangent : Mesh.Tangents)
			{
				EXPECT_NEAR(glm::length(glm::vec3(Tangent)), 1.0f, 1.0e-5f);
				EXPECT_NEAR(std::abs(Tangent.w), 1.0f, 1.0e-5f);
			}

			EXPECT_EQ(0u, Mesh.Indices[0]);
			EXPECT_EQ(1u, Mesh.Indices[1]);
			EXPECT_EQ(2u, Mesh.Indices[2]);
		}

		auto ExpectMeshEq(const FImportedMeshData& Expected, const FImportedMeshData& Actual) -> void
		{
			EXPECT_EQ(Expected.Name, Actual.Name);
			EXPECT_EQ(Expected.MaterialIndex, Actual.MaterialIndex);
			ASSERT_EQ(Expected.Positions.size(), Actual.Positions.size());
			ASSERT_EQ(Expected.Normals.size(), Actual.Normals.size());
			ASSERT_EQ(Expected.Tangents.size(), Actual.Tangents.size());
			ASSERT_EQ(Expected.Colors.size(), Actual.Colors.size());
			EXPECT_EQ(Expected.Indices, Actual.Indices);

			for (size_t Index = 0; Index < Expected.Positions.size(); ++Index)
			{
				ExpectVec3Eq(Expected.Positions[Index], Actual.Positions[Index]);
			}

			for (size_t Index = 0; Index < Expected.Normals.size(); ++Index)
			{
				ExpectVec3Eq(Expected.Normals[Index], Actual.Normals[Index]);
			}

			for (size_t Index = 0; Index < Expected.Tangents.size(); ++Index)
			{
				ExpectVec4Eq(Expected.Tangents[Index], Actual.Tangents[Index]);
			}
			for (uint32 Channel = 0; Channel < MaxImportedUVChannels; ++Channel)
			{
				ASSERT_EQ(Expected.UVChannels[Channel].size(), Actual.UVChannels[Channel].size());
				for (size_t Index = 0; Index < Expected.UVChannels[Channel].size(); ++Index)
				{
					ExpectVec2Eq(Expected.UVChannels[Channel][Index], Actual.UVChannels[Channel][Index]);
				}
			}
			for (size_t Index = 0; Index < Expected.Colors.size(); ++Index)
			{
				ExpectVec4Eq(Expected.Colors[Index], Actual.Colors[Index]);
			}
		}

		auto TestDataPath(std::string_view FileName) -> std::string
		{
			return (std::filesystem::path{DURIN_TEST_DATA_DIR} / std::string(FileName)).string();
		}
	}

	TEST(FAssetImportTests, ImportFromFileLoadsTriangleMesh)
	{
		FImportedSceneData Scene;

		ASSERT_TRUE(ImportFromFile(TestDataPath("Triangle.obj"), Scene));
		ASSERT_EQ(1u, Scene.Meshes.size());
		ASSERT_FALSE(Scene.MaterialSlots.empty());

		ExpectTriangleMesh(Scene.Meshes[0]);
	}

	TEST(FAssetImportTests, ImportsMaterialsMultipleUVsColorsAndNodeInstances)
	{
		FImportedSceneData Scene;
		ASSERT_TRUE(ImportFromFile(TestDataPath("MultiSection.gltf"), Scene));
		ASSERT_EQ(Scene.MaterialSlots.size(), 2u);
		EXPECT_EQ(Scene.MaterialSlots[0].Name, "Red");
		EXPECT_EQ(Scene.MaterialSlots[1].Name, "Blue");
		ASSERT_EQ(Scene.Meshes.size(), 4u);

		for (const FImportedMeshData& Mesh : Scene.Meshes)
		{
			EXPECT_EQ(Mesh.Positions.size(), 3u);
			EXPECT_EQ(Mesh.Normals.size(), 3u);
			EXPECT_EQ(Mesh.Tangents.size(), 3u);
			EXPECT_EQ(Mesh.UVChannels[0].size(), 3u);
			EXPECT_EQ(Mesh.UVChannels[1].size(), 3u);
			EXPECT_TRUE(Mesh.UVChannels[2].empty());
			EXPECT_TRUE(Mesh.UVChannels[3].empty());
			ASSERT_EQ(Mesh.Colors.size(), 3u);
			ExpectVec4Eq(glm::vec4(0.0f, 1.0f, 0.0f, 0.5f), Mesh.Colors[1]);
			for (size_t VertexIndex = 0; VertexIndex < Mesh.Tangents.size(); ++VertexIndex)
			{
				EXPECT_NEAR(glm::length(glm::vec3(Mesh.Tangents[VertexIndex])), 1.0f, 1.0e-5f);
				EXPECT_NEAR(glm::dot(Mesh.Normals[VertexIndex], glm::vec3(Mesh.Tangents[VertexIndex])), 0.0f, 1.0e-5f);
			}
		}

		EXPECT_EQ(Scene.Meshes[0].MaterialIndex, Scene.MaterialSlots[0].SourceMaterialIndex);
		EXPECT_EQ(Scene.Meshes[1].MaterialIndex, Scene.MaterialSlots[1].SourceMaterialIndex);
		ExpectVec3Eq(glm::vec3(1.0f, 2.0f, 3.0f), Scene.Meshes[0].Positions[0]);
		ExpectVec3Eq(glm::vec3(3.0f, 2.0f, 3.0f), Scene.Meshes[0].Positions[1]);
		EXPECT_EQ(Scene.Meshes[2].Indices, (std::vector<uint32>{0, 2, 1}));
		EXPECT_FLOAT_EQ(Scene.Meshes[2].Tangents[0].w, -1.0f);
	}

	TEST(FAssetImportTests, ImportFromFileAsyncMatchesSynchronousImport)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(2));

		FImportedSceneData SyncScene;
		ASSERT_TRUE(ImportFromFile(TestDataPath("Triangle.obj"), SyncScene));

		FAsyncMeshImportHandle Handle = ImportFromFileAsync(TestDataPath("Triangle.obj"));
		ASSERT_TRUE(Handle.IsValid());
		EXPECT_STREQ("AssetImport.Mesh", Handle.GetDebugName());
		EXPECT_FALSE(Handle.IsComplete());

		Handle.Wait();

		EXPECT_TRUE(Handle.IsComplete());

		FAsyncMeshImportResult AsyncResult;
		ASSERT_TRUE(Handle.TryGetResult(AsyncResult));
		ASSERT_TRUE(AsyncResult.bSucceeded);
		ASSERT_TRUE(AsyncResult.ErrorMessage.empty());
		ASSERT_EQ(SyncScene.Meshes.size(), AsyncResult.Scene.Meshes.size());
		ASSERT_EQ(SyncScene.MaterialSlots.size(), AsyncResult.Scene.MaterialSlots.size());
		ASSERT_EQ(1u, AsyncResult.Scene.Meshes.size());

		ExpectTriangleMesh(AsyncResult.Scene.Meshes[0]);
		ExpectMeshEq(SyncScene.Meshes[0], AsyncResult.Scene.Meshes[0]);
	}

	TEST(FAssetImportTests, ImportFromFileAsyncReportsImporterFailure)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(1));

		FAsyncMeshImportHandle Handle = ImportFromFileAsync("Missing/NoSuchMesh.obj");
		ASSERT_TRUE(Handle.IsValid());

		Handle.Wait();

		FAsyncMeshImportResult Result;
		ASSERT_TRUE(Handle.TryGetResult(Result));
		EXPECT_FALSE(Result.bSucceeded);
		EXPECT_TRUE(Result.Scene.Meshes.empty());
		EXPECT_FALSE(Result.ErrorMessage.empty());
	}

	TEST(FAssetImportTests, ImportFromFileAsyncReturnsInvalidHandleWhenThreadPoolStopped)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;

		FAsyncMeshImportHandle Handle = ImportFromFileAsync(TestDataPath("Triangle.obj"));
		EXPECT_FALSE(Handle.IsValid());
		EXPECT_FALSE(Handle.IsComplete());
		EXPECT_STREQ("", Handle.GetDebugName());
	}
}
