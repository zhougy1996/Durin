#include <gtest/gtest.h>

#include "AssetImport.h"
#include "Json/Json.h"
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

		auto MakeYUpNegativeZForwardOptions() -> FMeshImportOptions
		{
			FMeshImportOptions Options;
			Options.SourceToEngine = glm::mat4(0.0f);
			Options.SourceToEngine[2][0] = -1.0f;
			Options.SourceToEngine[0][1] = 1.0f;
			Options.SourceToEngine[1][2] = 1.0f;
			Options.SourceToEngine[3][3] = 1.0f;
			return Options;
		}

		auto WriteMaterialSlotFixture(
			std::string_view FileName,
			const std::vector<std::string>& MaterialNames,
			const std::vector<uint32>& PrimitiveMaterialIndices) -> std::filesystem::path
		{
			const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "MaterialSlotFixtures";
			std::filesystem::create_directories(Root);
			const std::filesystem::path Path = Root / FileName;
			std::ofstream Stream(Path, std::ios::trunc);
			EXPECT_TRUE(Stream.is_open());
			Stream << R"({
  "asset": { "version": "2.0" },
  "buffers": [{
    "byteLength": 224,
    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AACAPwAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAgD8AAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/zczMPc3MTD6amZk+zczMPgAAAD+amRk/AACAPwAAAAAAAAAAAACAPwAAAAAAAIA/AAAAAAAAAD8AAAAAAAAAAAAAgD8AAIA+AAABAAIAAAA="
  }],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 72, "byteLength": 48, "target": 34962 },
    { "buffer": 0, "byteOffset": 120, "byteLength": 24, "target": 34962 },
    { "buffer": 0, "byteOffset": 144, "byteLength": 24, "target": 34962 },
    { "buffer": 0, "byteOffset": 168, "byteLength": 48, "target": 34962 },
    { "buffer": 0, "byteOffset": 216, "byteLength": 8, "target": 34963 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" },
    { "bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC4" },
    { "bufferView": 6, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ],
  "materials": [)";
			for (size_t Index = 0; Index < MaterialNames.size(); ++Index)
			{
				if (Index != 0) Stream << ',';
				Stream << "\n    { \"name\": \"" << MaterialNames[Index] << "\" }";
			}
			Stream << R"(
  ],
  "meshes": [{
    "name": "MaterialSlots",
    "primitives": [)";
			for (size_t Index = 0; Index < PrimitiveMaterialIndices.size(); ++Index)
			{
				if (Index != 0) Stream << ',';
				Stream << "\n      { \"attributes\": { \"POSITION\": 0, \"NORMAL\": 1, \"TANGENT\": 2, \"TEXCOORD_0\": 3, \"TEXCOORD_1\": 4, \"COLOR_0\": 5 }, \"indices\": 6, \"material\": "
					<< PrimitiveMaterialIndices[Index] << " }";
			}
			Stream << R"(
    ]
  }],
  "nodes": [{ "name": "MaterialSlots", "mesh": 0 }],
  "scenes": [{ "nodes": [0] }],
  "scene": 0
}
)";
			EXPECT_TRUE(Stream.good());
			return Path;
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

	TEST(FAssetImportTests, MaterialSlotFixturesCharacterizeNamesOrderAndSourceIndices)
	{
		struct FCase
		{
			std::string FileName;
			std::vector<std::string> MaterialNames;
			std::vector<uint32> PrimitiveMaterialIndices;
			std::vector<FImportedMaterialSlot> ExpectedSlots;
		};
		const std::vector<FCase> Cases = {
			{"Base.gltf", {"Red", "Blue"}, {0, 1}, {{"Red", 0}, {"Blue", 1}}},
			{"Reordered.gltf", {"Blue", "Red"}, {0, 1}, {{"Blue", 0}, {"Red", 1}}},
			{"Renamed.gltf", {"Crimson", "Blue"}, {0, 1}, {{"Crimson", 0}, {"Blue", 1}}},
			{"Duplicate.gltf", {"Shared", "Shared"}, {0, 1}, {{"Shared", 0}, {"Shared_1", 1}}},
			{"Added.gltf", {"Red", "Green", "Blue"}, {0, 1, 2}, {{"Red", 0}, {"Green", 1}, {"Blue", 2}}},
			{"Removed.gltf", {"Blue"}, {0}, {{"Blue", 0}}},
			{"Filtered.gltf", {"Unused", "Red", "Blue"}, {1, 2}, {{"Red", 0}, {"Blue", 1}}},
			{"ExactNames.gltf", {"", " red ", "Red"}, {0, 1, 2}, {{"Material_0", 0}, {" red ", 1}, {"Red", 2}}},
		};

		for (const FCase& Case : Cases)
		{
			SCOPED_TRACE(Case.FileName);
			const std::filesystem::path Fixture = WriteMaterialSlotFixture(
				Case.FileName, Case.MaterialNames, Case.PrimitiveMaterialIndices);
			FImportedSceneData Scene;
			ASSERT_TRUE(ImportFromFile(Fixture.generic_string(), Scene));
			ASSERT_EQ(Scene.MaterialSlots.size(), Case.ExpectedSlots.size());
			for (size_t Index = 0; Index < Case.ExpectedSlots.size(); ++Index)
			{
				EXPECT_EQ(Scene.MaterialSlots[Index].Name, Case.ExpectedSlots[Index].Name);
				EXPECT_EQ(Scene.MaterialSlots[Index].SourceMaterialIndex, Case.ExpectedSlots[Index].SourceMaterialIndex);
				EXPECT_EQ(Scene.MaterialSlots[Index].SourceName, Case.MaterialNames[Case.PrimitiveMaterialIndices[Index]]);
			}
		}
	}

	TEST(FAssetImportTests, StaticModelMaterialContractFixturesRemainImportable)
	{
		struct FCase
		{
			std::string FileName;
			size_t ExpectedMeshCount;
			std::vector<std::string> ExpectedUsedMaterialNames;
		};
		const std::vector<FCase> Cases = {
			{"StaticModelMaterials/MaterialContract.gltf", 3, {"Shared", "Shared_1", "Blend"}},
			{"StaticModelMaterials/DataUriImage.gltf", 1, {"DataUri"}},
			{"StaticModelMaterials/EmbeddedImage.glb", 1, {"Embedded"}},
			{"StaticModelMaterials/OptionalExtension.gltf", 1, {"OptionalExtension"}},
			{"StaticModelMaterials/PhongMaterial.fbx", 1, {"phong1"}},
			{"StaticModelMaterials/UnsupportedDccMaterial.fbx", 1, {"02 - Default"}},
		};

		for (const FCase& Case : Cases)
		{
			SCOPED_TRACE(Case.FileName);
			FImportedSceneData Scene;
			ASSERT_TRUE(ImportFromFile(TestDataPath(Case.FileName), Scene));
			ASSERT_EQ(Scene.Meshes.size(), Case.ExpectedMeshCount);
			ASSERT_EQ(Scene.MaterialSlots.size(), Case.ExpectedUsedMaterialNames.size());
			for (size_t Index = 0; Index < Case.ExpectedUsedMaterialNames.size(); ++Index)
			{
				EXPECT_EQ(Scene.MaterialSlots[Index].Name, Case.ExpectedUsedMaterialNames[Index]);
			}
		}
	}

	TEST(FAssetImportTests, StaticModelGoldenSnapshotFreezesRequiredAndOptionalCases)
	{
		for (const std::string_view FileName : {
			"StaticModelMaterials/RequiredExtension.gltf",
			"StaticModelMaterials/OptionalExtension.gltf"})
		{
			SCOPED_TRACE(FileName);
			const std::filesystem::path Path = TestDataPath(FileName);
			ASSERT_TRUE(std::filesystem::is_regular_file(Path));
			EXPECT_GT(std::filesystem::file_size(Path), 0u);
		}

		FJsonDocument Contract;
		FJsonParseError ParseError;
		ASSERT_TRUE(Contract.LoadFromFile(
			TestDataPath("StaticModelMaterials/ExpectedNormalized.json"), &ParseError)) << ParseError.Message;
		const FJsonNodeView Root = Contract.GetRootView();
		EXPECT_EQ(Root.GetView("contractVersion").GetInt(), 1);
		const FJsonNodeView Fixtures = Root.GetView("fixtures");
		ASSERT_TRUE(Fixtures.IsObject());
		EXPECT_EQ(Fixtures.GetView("RequiredExtension.gltf").GetView("result").GetString(), "failure");
		EXPECT_EQ(Fixtures.GetView("OptionalExtension.gltf").GetView("result").GetString(), "success");
		EXPECT_EQ(
			Fixtures.GetView("MaterialContract.gltf").GetView("usedSlots").Num(),
			3u);
		EXPECT_EQ(
			Fixtures.GetView("EmbeddedImage.glb").GetView("images").GetView(0).GetView("identity").GetString(),
			"glb-buffer-view:4");
	}

	TEST(FAssetImportTests, AppliesSourceCoordinateSystemToAllMeshAttributes)
	{
		FImportedSceneData Scene;
		ASSERT_TRUE(ImportFromFile(TestDataPath("AsymmetricAxes.obj"), Scene, MakeYUpNegativeZForwardOptions()));
		ASSERT_EQ(Scene.Meshes.size(), 1u);
		const FImportedMeshData& Mesh = Scene.Meshes[0];
		ASSERT_EQ(Mesh.Positions.size(), 3u);
		ASSERT_EQ(Mesh.Normals.size(), 3u);
		ASSERT_EQ(Mesh.Tangents.size(), 3u);

		ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 0.0f), Mesh.Positions[0]);
		ExpectVec3Eq(glm::vec3(0.0f, 1.0f, 0.0f), Mesh.Positions[1]);
		ExpectVec3Eq(glm::vec3(0.0f, 0.0f, 1.0f), Mesh.Positions[2]);
		for (size_t VertexIndex = 0; VertexIndex < Mesh.Positions.size(); ++VertexIndex)
		{
			ExpectVec3Eq(glm::vec3(-1.0f, 0.0f, 0.0f), Mesh.Normals[VertexIndex]);
			ExpectVec3Eq(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(Mesh.Tangents[VertexIndex]));
			EXPECT_FLOAT_EQ(Mesh.Tangents[VertexIndex].w, -1.0f);
		}
		EXPECT_EQ(Mesh.Indices, (std::vector<uint32>{0, 2, 1}));
	}

	TEST(FAssetImportTests, AsyncImportAppliesSourceCoordinateSystem)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(1));

		FAsyncMeshImportHandle Handle = ImportFromFileAsync(TestDataPath("AsymmetricAxes.obj"), MakeYUpNegativeZForwardOptions());
		ASSERT_TRUE(Handle.IsValid());
		Handle.Wait();

		FAsyncMeshImportResult Result;
		ASSERT_TRUE(Handle.TryGetResult(Result));
		ASSERT_TRUE(Result.bSucceeded);
		ASSERT_EQ(Result.Scene.Meshes.size(), 1u);
		ExpectVec3Eq(glm::vec3(0.0f, 1.0f, 0.0f), Result.Scene.Meshes[0].Positions[1]);
		ExpectVec3Eq(glm::vec3(-1.0f, 0.0f, 0.0f), Result.Scene.Meshes[0].Normals[0]);
		EXPECT_EQ(Result.Scene.Meshes[0].Indices, (std::vector<uint32>{0, 2, 1}));
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
