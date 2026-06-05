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

		auto ExpectTriangleMesh(const FTestAssetData& Mesh) -> void
		{
			ASSERT_EQ(3u, Mesh.Positions.size());
			ASSERT_EQ(3u, Mesh.Normals.size());
			ASSERT_EQ(3u, Mesh.UVs.size());
			ASSERT_EQ(3u, Mesh.Indices.size());

			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 0.0f), Mesh.Positions[0]);
			ExpectVec3Eq(glm::vec3(1.0f, 0.0f, 0.0f), Mesh.Positions[1]);
			ExpectVec3Eq(glm::vec3(0.0f, 1.0f, 0.0f), Mesh.Positions[2]);

			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 1.0f), Mesh.Normals[0]);
			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 1.0f), Mesh.Normals[1]);
			ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 1.0f), Mesh.Normals[2]);

			ExpectVec2Eq(glm::vec2(0.0f, 1.0f), Mesh.UVs[0]);
			ExpectVec2Eq(glm::vec2(1.0f, 1.0f), Mesh.UVs[1]);
			ExpectVec2Eq(glm::vec2(0.0f, 0.0f), Mesh.UVs[2]);

			EXPECT_EQ(0u, Mesh.Indices[0]);
			EXPECT_EQ(1u, Mesh.Indices[1]);
			EXPECT_EQ(2u, Mesh.Indices[2]);
		}

		auto ExpectMeshEq(const FTestAssetData& Expected, const FTestAssetData& Actual) -> void
		{
			ASSERT_EQ(Expected.Positions.size(), Actual.Positions.size());
			ASSERT_EQ(Expected.Normals.size(), Actual.Normals.size());
			ASSERT_EQ(Expected.UVs.size(), Actual.UVs.size());
			EXPECT_EQ(Expected.Indices, Actual.Indices);

			for (size_t Index = 0; Index < Expected.Positions.size(); ++Index)
			{
				ExpectVec3Eq(Expected.Positions[Index], Actual.Positions[Index]);
			}

			for (size_t Index = 0; Index < Expected.Normals.size(); ++Index)
			{
				ExpectVec3Eq(Expected.Normals[Index], Actual.Normals[Index]);
			}

			for (size_t Index = 0; Index < Expected.UVs.size(); ++Index)
			{
				ExpectVec2Eq(Expected.UVs[Index], Actual.UVs[Index]);
			}
		}

		auto TestDataPath(std::string_view FileName) -> std::string
		{
			return (std::filesystem::path{DURIN_TEST_DATA_DIR} / std::string(FileName)).string();
		}
	}

	TEST(FAssetImportTests, ImportFromFileLoadsTriangleMesh)
	{
		std::vector<FTestAssetData> Meshes;

		ASSERT_TRUE(ImportFromFile(TestDataPath("Triangle.obj"), Meshes));
		ASSERT_EQ(1u, Meshes.size());

		ExpectTriangleMesh(Meshes[0]);
	}

	TEST(FAssetImportTests, ImportFromFileAsyncMatchesSynchronousImport)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(2));

		std::vector<FTestAssetData> SyncMeshes;
		ASSERT_TRUE(ImportFromFile(TestDataPath("Triangle.obj"), SyncMeshes));

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
		ASSERT_EQ(SyncMeshes.size(), AsyncResult.Meshes.size());
		ASSERT_EQ(1u, AsyncResult.Meshes.size());

		ExpectTriangleMesh(AsyncResult.Meshes[0]);
		ExpectMeshEq(SyncMeshes[0], AsyncResult.Meshes[0]);
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
		EXPECT_TRUE(Result.Meshes.empty());
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
