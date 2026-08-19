#include <gtest/gtest.h>

#include "ImportedScene.h"
#include "Json/Json.h"
#include "Math/Operations.h"
#include "NativeTestSupport.h"

namespace Durin::Asset
{
	using namespace Forge;

	namespace
	{
		auto ExpectVec3Eq(const FVector3f& Expected, const FVector3f& Actual) -> void
		{
			EXPECT_FLOAT_EQ(Expected.x, Actual.x);
			EXPECT_FLOAT_EQ(Expected.y, Actual.y);
			EXPECT_FLOAT_EQ(Expected.z, Actual.z);
		}

		auto ExpectVec2Eq(const FVector2f& Expected, const FVector2f& Actual) -> void
		{
			EXPECT_FLOAT_EQ(Expected.x, Actual.x);
			EXPECT_FLOAT_EQ(Expected.y, Actual.y);
		}

		auto ExpectVec4Eq(const FVector4f& Expected, const FVector4f& Actual) -> void
		{
			EXPECT_FLOAT_EQ(Expected.x, Actual.x);
			EXPECT_FLOAT_EQ(Expected.y, Actual.y);
			EXPECT_FLOAT_EQ(Expected.z, Actual.z);
			EXPECT_FLOAT_EQ(Expected.w, Actual.w);
		}

		auto ExpectMatrixEq(const FMatrix4f& Expected, const FMatrix4f& Actual) -> void
		{
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					EXPECT_FLOAT_EQ(Expected[Column][Row], Actual[Column][Row]);
		}

		auto ExpectTriangleMesh(const FImportedMeshData& Mesh) -> void
		{
			ASSERT_EQ(3u, Mesh.Positions.size());
			ASSERT_EQ(3u, Mesh.Normals.size());
			ASSERT_EQ(3u, Mesh.Tangents.size());
			ASSERT_EQ(3u, Mesh.UVChannels[0].size());
			ASSERT_EQ(3u, Mesh.Indices.size());

			ExpectVec3Eq(FVector3f(0.0f, 0.0f, 0.0f), Mesh.Positions[0]);
			ExpectVec3Eq(FVector3f(1.0f, 0.0f, 0.0f), Mesh.Positions[1]);
			ExpectVec3Eq(FVector3f(0.0f, 1.0f, 0.0f), Mesh.Positions[2]);

			ExpectVec3Eq(FVector3f(0.0f, 0.0f, 1.0f), Mesh.Normals[0]);
			ExpectVec3Eq(FVector3f(0.0f, 0.0f, 1.0f), Mesh.Normals[1]);
			ExpectVec3Eq(FVector3f(0.0f, 0.0f, 1.0f), Mesh.Normals[2]);

			ExpectVec2Eq(FVector2f(0.0f, 1.0f), Mesh.UVChannels[0][0]);
			ExpectVec2Eq(FVector2f(1.0f, 1.0f), Mesh.UVChannels[0][1]);
			ExpectVec2Eq(FVector2f(0.0f, 0.0f), Mesh.UVChannels[0][2]);
			for (const FVector4f& Tangent : Mesh.Tangents)
			{
				EXPECT_NEAR(Math::Length(FVector3f(Tangent)), 1.0f, 1.0e-5f);
				EXPECT_NEAR(std::abs(Tangent.w), 1.0f, 1.0e-5f);
			}

			EXPECT_EQ(0u, Mesh.Indices[0]);
			EXPECT_EQ(1u, Mesh.Indices[1]);
			EXPECT_EQ(2u, Mesh.Indices[2]);
		}

		auto ExpectMeshEq(const FImportedMeshData& Expected, const FImportedMeshData& Actual) -> void
		{
			EXPECT_EQ(Expected.Name, Actual.Name);
			EXPECT_EQ(Expected.SourceMaterialIndex, Actual.SourceMaterialIndex);
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

		auto HasDiagnostic(
			const FImportedSceneData& Scene,
			EImportDiagnosticSeverity Severity,
			ESceneImportDiagnosticCategory Category,
			std::string_view Subject) -> bool
		{
			return std::ranges::any_of(Scene.Diagnostics, [=](const FSceneImportDiagnostic& Diagnostic) {
				return Diagnostic.Severity == Severity &&
					Diagnostic.Category == Category &&
					Diagnostic.Subject == Subject;
			});
		}

		auto ExpectNormalizedSceneEq(const FImportedSceneData& Expected, const FImportedSceneData& Actual) -> void
		{
			ASSERT_EQ(Expected.Images.size(), Actual.Images.size());
			for (size_t Index = 0; Index < Expected.Images.size(); ++Index)
			{
				const FImportedImage& A = Expected.Images[Index];
				const FImportedImage& B = Actual.Images[Index];
				EXPECT_EQ(A.StableIdentity, B.StableIdentity);
				EXPECT_EQ(A.SuggestedName, B.SuggestedName);
				EXPECT_EQ(A.Encoding, B.Encoding);
				EXPECT_EQ(A.EncodedByteCount, B.EncodedByteCount);
				EXPECT_EQ(A.ExternalDependencyIndex, B.ExternalDependencyIndex);
				EXPECT_EQ(A.EmbeddedEncodedBytes, B.EmbeddedEncodedBytes);
			}
			ASSERT_EQ(Expected.Materials.size(), Actual.Materials.size());
			for (size_t Index = 0; Index < Expected.Materials.size(); ++Index)
			{
				const FImportedMaterial& A = Expected.Materials[Index];
				const FImportedMaterial& B = Actual.Materials[Index];
				EXPECT_EQ(A.SourceMaterialIndex, B.SourceMaterialIndex);
				EXPECT_EQ(A.SourceName, B.SourceName);
				ExpectVec4Eq(A.BaseColorFactor, B.BaseColorFactor);
				EXPECT_FLOAT_EQ(A.MetallicFactor, B.MetallicFactor);
				EXPECT_FLOAT_EQ(A.RoughnessFactor, B.RoughnessFactor);
				ExpectVec3Eq(A.EmissiveFactor, B.EmissiveFactor);
				EXPECT_EQ(A.AlphaMode, B.AlphaMode);
				EXPECT_FLOAT_EQ(A.AlphaCutoff, B.AlphaCutoff);
				EXPECT_EQ(A.bDoubleSided, B.bDoubleSided);
				ASSERT_EQ(A.TextureBindings.size(), B.TextureBindings.size());
				for (size_t BindingIndex = 0; BindingIndex < A.TextureBindings.size(); ++BindingIndex)
				{
					const FImportedTextureBinding& X = A.TextureBindings[BindingIndex];
					const FImportedTextureBinding& Y = B.TextureBindings[BindingIndex];
					EXPECT_EQ(X.Semantic, Y.Semantic);
					EXPECT_EQ(X.ImageIndex, Y.ImageIndex);
					EXPECT_EQ(X.UVChannel, Y.UVChannel);
					ExpectVec2Eq(X.Offset, Y.Offset);
					ExpectVec2Eq(X.Scale, Y.Scale);
					EXPECT_FLOAT_EQ(X.RotationRadians, Y.RotationRadians);
					EXPECT_FLOAT_EQ(X.Strength, Y.Strength);
					EXPECT_EQ(X.Sampler.MinFilter, Y.Sampler.MinFilter);
					EXPECT_EQ(X.Sampler.MagFilter, Y.Sampler.MagFilter);
					EXPECT_EQ(X.Sampler.WrapU, Y.Sampler.WrapU);
					EXPECT_EQ(X.Sampler.WrapV, Y.Sampler.WrapV);
				}
			}
			ASSERT_EQ(Expected.MaterialSlots.size(), Actual.MaterialSlots.size());
			for (size_t Index = 0; Index < Expected.MaterialSlots.size(); ++Index)
			{
				EXPECT_EQ(Expected.MaterialSlots[Index].Name, Actual.MaterialSlots[Index].Name);
				EXPECT_EQ(Expected.MaterialSlots[Index].SourceMaterialIndex, Actual.MaterialSlots[Index].SourceMaterialIndex);
				EXPECT_EQ(Expected.MaterialSlots[Index].SourceName, Actual.MaterialSlots[Index].SourceName);
			}
			ASSERT_EQ(Expected.Meshes.size(), Actual.Meshes.size());
			for (size_t Index = 0; Index < Expected.Meshes.size(); ++Index)
				ExpectMeshEq(Expected.Meshes[Index], Actual.Meshes[Index]);
			ASSERT_EQ(Expected.Dependencies.size(), Actual.Dependencies.size());
			for (size_t Index = 0; Index < Expected.Dependencies.size(); ++Index)
			{
				EXPECT_EQ(Expected.Dependencies[Index].Role, Actual.Dependencies[Index].Role);
				EXPECT_EQ(Expected.Dependencies[Index].StableIdentity, Actual.Dependencies[Index].StableIdentity);
				EXPECT_EQ(Expected.Dependencies[Index].Source, Actual.Dependencies[Index].Source);
				EXPECT_EQ(Expected.Dependencies[Index].ContentHash, Actual.Dependencies[Index].ContentHash);
				EXPECT_EQ(Expected.Dependencies[Index].ByteCount, Actual.Dependencies[Index].ByteCount);
			}
			ASSERT_EQ(Expected.Diagnostics.size(), Actual.Diagnostics.size());
			for (size_t Index = 0; Index < Expected.Diagnostics.size(); ++Index)
			{
				EXPECT_EQ(Expected.Diagnostics[Index].Severity, Actual.Diagnostics[Index].Severity);
				EXPECT_EQ(Expected.Diagnostics[Index].Category, Actual.Diagnostics[Index].Category);
				EXPECT_EQ(Expected.Diagnostics[Index].SourceIdentity, Actual.Diagnostics[Index].SourceIdentity);
				EXPECT_EQ(Expected.Diagnostics[Index].Subject, Actual.Diagnostics[Index].Subject);
				EXPECT_EQ(Expected.Diagnostics[Index].Message, Actual.Diagnostics[Index].Message);
			}
		}

		auto TestDataPath(std::string_view FileName) -> std::string
		{
			return (std::filesystem::path{DURIN_TEST_DATA_DIR} / std::string(FileName)).string();
		}

		auto ReplaceAll(std::string& Value, std::string_view From, std::string_view To) -> size_t
		{
			size_t Count = 0;
			for (size_t Offset = 0; (Offset = Value.find(From, Offset)) != std::string::npos; ++Count)
			{
				Value.replace(Offset, From.size(), To);
				Offset += To.size();
			}
			return Count;
		}

		auto WriteOptionalSkeletalAttributesFixture() -> std::filesystem::path
		{
			std::ifstream Source(TestDataPath("Skeletal/Contract.gltf"), std::ios::binary);
			EXPECT_TRUE(Source.is_open());
			std::stringstream Stream;
			Stream << Source.rdbuf();
			std::string Document = Stream.str();
			EXPECT_EQ(ReplaceAll(Document,
				"            \"NORMAL\": 1,\n",
				"            \"COLOR_0\": 1,\n"), 2u);
			EXPECT_EQ(ReplaceAll(Document, "            \"TANGENT\": 2,\n", ""), 2u);
			EXPECT_EQ(ReplaceAll(Document, "          \"material\": 0,\n", ""), 2u);

			const std::filesystem::path Root =
				Durin::Testing::GetTestWorkDirectory() / "OptionalSkeletalAttributes";
			std::filesystem::create_directories(Root);
			const std::filesystem::path Path = Root / "OptionalAttributes.gltf";
			std::ofstream Destination(Path, std::ios::binary | std::ios::trunc);
			EXPECT_TRUE(Destination.is_open());
			Destination.write(Document.data(), static_cast<std::streamsize>(Document.size()));
			EXPECT_TRUE(Destination.good());
			return Path;
		}

		auto MakeYUpNegativeZForwardOptions() -> FMeshImportOptions
		{
			FMeshImportOptions Options;
			Options.SourceToEngine = FMatrix4f(0.0f);
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
			const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "MaterialSlotFixtures";
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
			ExpectVec4Eq(FVector4f(0.0f, 1.0f, 0.0f, 0.5f), Mesh.Colors[1]);
			for (size_t VertexIndex = 0; VertexIndex < Mesh.Tangents.size(); ++VertexIndex)
			{
				EXPECT_NEAR(Math::Length(FVector3f(Mesh.Tangents[VertexIndex])), 1.0f, 1.0e-5f);
				EXPECT_NEAR(
					Math::Dot(Mesh.Normals[VertexIndex], FVector3f(Mesh.Tangents[VertexIndex])), 0.0f, 1.0e-5f);
			}
		}

		EXPECT_EQ(Scene.Meshes[0].SourceMaterialIndex, Scene.MaterialSlots[0].SourceMaterialIndex);
		EXPECT_EQ(Scene.Meshes[1].SourceMaterialIndex, Scene.MaterialSlots[1].SourceMaterialIndex);
		ExpectVec3Eq(FVector3f(1.0f, 2.0f, 3.0f), Scene.Meshes[0].Positions[0]);
		ExpectVec3Eq(FVector3f(3.0f, 2.0f, 3.0f), Scene.Meshes[0].Positions[1]);
		EXPECT_EQ(Scene.Meshes[2].Indices, (std::vector<uint32>{0, 2, 1}));
		EXPECT_FLOAT_EQ(Scene.Meshes[2].Tangents[0].w, 1.0f);
	}

	TEST(FAssetImportTests, SkeletalContractContainersPreserveCurrentStaticBaseline)
	{
		std::array<FImportedSceneData, 3> Scenes;
		const std::array<std::string_view, 3> Files = {
			"Skeletal/StaticProjection.gltf",
			"Skeletal/StaticProjectionExternal.gltf",
			"Skeletal/StaticProjection.glb"};
		for (size_t Index = 0; Index < Files.size(); ++Index)
		{
			SCOPED_TRACE(Files[Index]);
			ASSERT_TRUE(ImportFromFile(TestDataPath(Files[Index]), Scenes[Index]));
			ASSERT_EQ(Scenes[Index].MaterialSlots.size(), 2u);
			EXPECT_EQ(Scenes[Index].MaterialSlots[0].Name, "Red");
			EXPECT_EQ(Scenes[Index].MaterialSlots[1].Name, "Blue");
			ASSERT_EQ(Scenes[Index].Meshes.size(), 4u);
			EXPECT_TRUE(Scenes[Index].Diagnostics.empty());
			for (const FImportedMeshData& Mesh : Scenes[Index].Meshes)
			{
				EXPECT_EQ(Mesh.Positions.size(), 3u);
				EXPECT_EQ(Mesh.Normals.size(), 3u);
				EXPECT_EQ(Mesh.Tangents.size(), 3u);
				EXPECT_EQ(Mesh.UVChannels[0].size(), 3u);
				EXPECT_EQ(Mesh.Indices, (std::vector<uint32>{0, 1, 2}));
			}
		}

		for (size_t SceneIndex = 1; SceneIndex < Scenes.size(); ++SceneIndex)
		{
			ASSERT_EQ(Scenes[0].MaterialSlots.size(), Scenes[SceneIndex].MaterialSlots.size());
			for (size_t SlotIndex = 0; SlotIndex < Scenes[0].MaterialSlots.size(); ++SlotIndex)
			{
				EXPECT_EQ(Scenes[0].MaterialSlots[SlotIndex].Name,
					Scenes[SceneIndex].MaterialSlots[SlotIndex].Name);
				EXPECT_EQ(Scenes[0].MaterialSlots[SlotIndex].SourceMaterialIndex,
					Scenes[SceneIndex].MaterialSlots[SlotIndex].SourceMaterialIndex);
			}
			ASSERT_EQ(Scenes[0].Meshes.size(), Scenes[SceneIndex].Meshes.size());
			for (size_t MeshIndex = 0; MeshIndex < Scenes[0].Meshes.size(); ++MeshIndex)
				ExpectMeshEq(Scenes[0].Meshes[MeshIndex], Scenes[SceneIndex].Meshes[MeshIndex]);
		}
	}

	TEST(FAssetImportTests, SkeletalContractContainersNormalizeExactRuntimeValues)
	{
		const std::array<std::string_view, 3> Files = {
			"Skeletal/Contract.gltf",
			"Skeletal/ContractExternal.gltf",
			"Skeletal/Contract.glb"};
		const std::array<std::string_view, 3> ProjectionFiles = {
			"Skeletal/StaticProjection.gltf",
			"Skeletal/StaticProjectionExternal.gltf",
			"Skeletal/StaticProjection.glb"};
		std::array<FImportedSceneData, 3> Scenes;
		for (size_t ContainerIndex = 0; ContainerIndex < Files.size(); ++ContainerIndex)
		{
			SCOPED_TRACE(Files[ContainerIndex]);
			ASSERT_TRUE(ImportFromFile(TestDataPath(Files[ContainerIndex]), Scenes[ContainerIndex]));
			const FImportedSceneData& Scene = Scenes[ContainerIndex];
			ASSERT_EQ(Scene.Nodes.size(), 7u);
			ASSERT_EQ(Scene.Skeletons.size(), 2u);
			ASSERT_EQ(Scene.SkeletalMeshes.size(), 2u);
			ASSERT_EQ(Scene.AnimationClips.size(), 4u);
			for (size_t SkeletonIndex = 0; SkeletonIndex < Scene.Skeletons.size(); ++SkeletonIndex)
			{
				const FImportedSkeletonData& Skeleton = Scene.Skeletons[SkeletonIndex];
				EXPECT_EQ(Skeleton.StableIdentity, std::format("skeleton:skin/{}", SkeletonIndex));
				EXPECT_EQ(Skeleton.CompatibilityIdentity, "be0f679ef83133e5acfab7f12b688f54");
				ASSERT_EQ(Skeleton.Bones.size(), 5u);
				EXPECT_EQ(Skeleton.Bones[0].Name, FName("$DurinRoot"));
				EXPECT_EQ(Skeleton.Bones[0].ParentIndex, -1);
				EXPECT_EQ(Skeleton.Bones[1].Name, FName("Hip"));
				EXPECT_EQ(Skeleton.Bones[1].ParentIndex, 0);
				EXPECT_EQ(Skeleton.Bones[2].Name, FName("Knee"));
				EXPECT_EQ(Skeleton.Bones[2].ParentIndex, 1);
				EXPECT_EQ(Skeleton.Bones[3].Name, FName("Shoulder"));
				EXPECT_EQ(Skeleton.Bones[3].ParentIndex, 0);
				EXPECT_EQ(Skeleton.Bones[4].Name, FName("Hand"));
				EXPECT_EQ(Skeleton.Bones[4].ParentIndex, 3);
				FMatrix4f ExpectedHip(1.0f);
				ExpectedHip[0][0] = 0.7071068f;
				ExpectedHip[0][1] = -0.7071068f;
				ExpectedHip[1][0] = 0.7071068f;
				ExpectedHip[1][1] = 0.7071068f;
				ExpectedHip[3][2] = 1.0f;
				ExpectMatrixEq(ExpectedHip, Skeleton.Bones[1].ReferenceTransform.ToMatrix4f());
			}

			ASSERT_EQ(Scene.SkeletalMeshes[0].StableIdentity, "skeletal-mesh:node/1/mesh/0");
			ASSERT_EQ(Scene.SkeletalMeshes[1].StableIdentity, "skeletal-mesh:node/6/mesh/1");
			for (const FImportedSkeletalMeshData& Mesh : Scene.SkeletalMeshes)
			{
				ASSERT_NE(Mesh.Payload, nullptr);
				EXPECT_EQ(Mesh.Payload->Positions.size(), 6u);
				EXPECT_EQ(Mesh.Payload->Indices.size(), 6u);
				EXPECT_EQ(Mesh.Payload->Indices, (std::vector<uint32>{0, 2, 1, 3, 5, 4}));
				EXPECT_EQ(Mesh.Payload->Sections.size(), 2u);
				EXPECT_EQ(Mesh.Payload->PaletteBoneIndices, (std::vector<uint16>{1, 2, 3, 4}));
				EXPECT_EQ(Mesh.Payload->InverseBindMatrices.size(), 4u);
				EXPECT_EQ(Mesh.MaterialSlots.size(), 2u);
			}
			const FSkeletalMeshVertexInfluences& FirstInfluence =
				Scene.SkeletalMeshes[0].Payload->Influences[0];
			EXPECT_EQ(FirstInfluence.Count, 4u);
			EXPECT_EQ(FirstInfluence.BoneIndices,
				(std::array<uint16, MaximumSkeletalMeshInfluences>{1, 2, 3, 4}));
			EXPECT_FLOAT_EQ(FirstInfluence.Weights[0], 0.4f);
			EXPECT_FLOAT_EQ(FirstInfluence.Weights[1], 0.3f);
			EXPECT_FLOAT_EQ(FirstInfluence.Weights[2], 0.2f);
			EXPECT_FLOAT_EQ(FirstInfluence.Weights[3], 0.1f);

			EXPECT_EQ(Scene.AnimationClips[0].StableIdentity, "animation-clip:animation/0/skin/0");
			EXPECT_EQ(Scene.AnimationClips[1].StableIdentity, "animation-clip:animation/0/skin/1");
			EXPECT_EQ(Scene.AnimationClips[2].StableIdentity, "animation-clip:animation/1/skin/0");
			EXPECT_EQ(Scene.AnimationClips[3].StableIdentity, "animation-clip:animation/1/skin/1");
			ASSERT_NE(Scene.AnimationClips[0].Payload, nullptr);
			EXPECT_FLOAT_EQ(Scene.AnimationClips[0].Payload->DurationSeconds, 2.0f);
			ASSERT_EQ(Scene.AnimationClips[0].Payload->Tracks.size(), 3u);
			EXPECT_EQ(Scene.AnimationClips[0].Payload->Tracks[0].BoneIndex, 1u);
			EXPECT_EQ(Scene.AnimationClips[0].Payload->Tracks[0].Path, EAnimationTrackPath::Translation);
			EXPECT_EQ(Scene.AnimationClips[0].Payload->Tracks[1].Interpolation, EAnimationInterpolation::Step);
			ASSERT_EQ(Scene.AnimationClips[0].Payload->Tracks[1].RotationValues.size(), 3u);
			ExpectVec4Eq(FVector4f(0.7071068f, 0.0f, 0.0f, 0.7071068f),
				Scene.AnimationClips[0].Payload->Tracks[1].RotationValues[1]);

			FImportedSceneData Projection;
			ASSERT_TRUE(ImportFromFile(TestDataPath(ProjectionFiles[ContainerIndex]), Projection));
			ASSERT_EQ(Scene.Meshes.size(), Projection.Meshes.size());
			for (size_t MeshIndex = 0; MeshIndex < Scene.Meshes.size(); ++MeshIndex)
				ExpectMeshEq(Projection.Meshes[MeshIndex], Scene.Meshes[MeshIndex]);
		}

		for (size_t ContainerIndex = 1; ContainerIndex < Scenes.size(); ++ContainerIndex)
		{
			ASSERT_EQ(Scenes[0].Skeletons.size(), Scenes[ContainerIndex].Skeletons.size());
			for (size_t SkeletonIndex = 0; SkeletonIndex < Scenes[0].Skeletons.size(); ++SkeletonIndex)
			{
				EXPECT_EQ(Scenes[0].Skeletons[SkeletonIndex].StableIdentity,
					Scenes[ContainerIndex].Skeletons[SkeletonIndex].StableIdentity);
				EXPECT_EQ(Scenes[0].Skeletons[SkeletonIndex].CompatibilityIdentity,
					Scenes[ContainerIndex].Skeletons[SkeletonIndex].CompatibilityIdentity);
				EXPECT_EQ(Scenes[0].Skeletons[SkeletonIndex].Bones,
					Scenes[ContainerIndex].Skeletons[SkeletonIndex].Bones);
			}
			ASSERT_EQ(Scenes[0].SkeletalMeshes.size(), Scenes[ContainerIndex].SkeletalMeshes.size());
			for (size_t MeshIndex = 0; MeshIndex < Scenes[0].SkeletalMeshes.size(); ++MeshIndex)
			{
				EXPECT_EQ(Scenes[0].SkeletalMeshes[MeshIndex].StableIdentity,
					Scenes[ContainerIndex].SkeletalMeshes[MeshIndex].StableIdentity);
				ASSERT_NE(Scenes[0].SkeletalMeshes[MeshIndex].Payload, nullptr);
				ASSERT_NE(Scenes[ContainerIndex].SkeletalMeshes[MeshIndex].Payload, nullptr);
				EXPECT_EQ(*Scenes[0].SkeletalMeshes[MeshIndex].Payload,
					*Scenes[ContainerIndex].SkeletalMeshes[MeshIndex].Payload);
			}
			ASSERT_EQ(Scenes[0].AnimationClips.size(), Scenes[ContainerIndex].AnimationClips.size());
			for (size_t ClipIndex = 0; ClipIndex < Scenes[0].AnimationClips.size(); ++ClipIndex)
			{
				EXPECT_EQ(Scenes[0].AnimationClips[ClipIndex].StableIdentity,
					Scenes[ContainerIndex].AnimationClips[ClipIndex].StableIdentity);
				ASSERT_NE(Scenes[0].AnimationClips[ClipIndex].Payload, nullptr);
				ASSERT_NE(Scenes[ContainerIndex].AnimationClips[ClipIndex].Payload, nullptr);
				EXPECT_EQ(*Scenes[0].AnimationClips[ClipIndex].Payload,
					*Scenes[ContainerIndex].AnimationClips[ClipIndex].Payload);
			}
		}
	}

	TEST(FAssetImportTests, SkeletalGltfGeneratesMissingBasisAndAcceptsDefaultMaterialAndRgbColors)
	{
		FImportedSceneData Scene;
		ASSERT_TRUE(ImportFromFile(WriteOptionalSkeletalAttributesFixture().generic_string(), Scene));
		ASSERT_EQ(Scene.Materials.size(), 3u);
		EXPECT_EQ(Scene.Materials[2].SourceMaterialIndex, 2u);
		EXPECT_EQ(Scene.Materials[2].SourceName, "Default");
		ASSERT_EQ(Scene.SkeletalMeshes.size(), 2u);

		for (const FImportedSkeletalMeshData& Mesh : Scene.SkeletalMeshes)
		{
			ASSERT_NE(Mesh.Payload, nullptr);
			EXPECT_EQ(Mesh.Payload->Indices, (std::vector<uint32>{0, 2, 1, 3, 5, 4}));
			ASSERT_EQ(Mesh.MaterialSlots.size(), 2u);
			EXPECT_EQ(Mesh.MaterialSlots[0].SourceMaterialIndex, 2u);
			EXPECT_EQ(Mesh.MaterialSlots[0].SourceName, "Default");
			ASSERT_EQ(Mesh.Payload->Normals.size(), 6u);
			ASSERT_EQ(Mesh.Payload->Tangents.size(), 6u);
			ASSERT_EQ(Mesh.Payload->Colors.size(), 6u);
			for (size_t Vertex = 0; Vertex < 3; ++Vertex)
			{
				ExpectVec3Eq(FVector3f(-1.0f, 0.0f, 0.0f), Mesh.Payload->Normals[Vertex]);
				ExpectVec4Eq(FVector4f(0.0f, 1.0f, 0.0f, -1.0f), Mesh.Payload->Tangents[Vertex]);
				ExpectVec4Eq(FVector4f(0.0f, 0.0f, 1.0f, 1.0f), Mesh.Payload->Colors[Vertex]);
			}
		}

		ASSERT_EQ(Scene.Meshes.size(), 4u);
		EXPECT_EQ(Scene.Meshes[0].SourceMaterialIndex, 2u);
		EXPECT_TRUE(std::ranges::any_of(Scene.MaterialSlots, [](const FImportedMaterialSlot& Slot) {
			return Slot.SourceMaterialIndex == 2u && Slot.Name == "Default";
		}));
	}

	TEST(FAssetImportTests, SkeletalMalformedFixturesUseFrozenDiagnosticCategories)
	{
		struct FCase
		{
			std::string_view File;
			ESceneImportDiagnosticCategory Category;
		};
		const std::array Cases = {
			FCase{"CyclicHierarchy.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"DisconnectedHierarchy.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"CountMismatch.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"InvalidAnimationTarget.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"UnsupportedCubicSpline.gltf", ESceneImportDiagnosticCategory::UnsupportedFeature},
			FCase{"UnsupportedSecondaryInfluences.gltf", ESceneImportDiagnosticCategory::UnsupportedFeature},
			FCase{"UnsupportedRequiredExtension.gltf", ESceneImportDiagnosticCategory::UnsupportedFeature},
			FCase{"ResourceLimit.gltf", ESceneImportDiagnosticCategory::ResourceLimitExceeded},
			FCase{"SparseAccessor.gltf", ESceneImportDiagnosticCategory::UnsupportedFeature},
			FCase{"TruncatedAccessor.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"AnimatedNonJoint.gltf", ESceneImportDiagnosticCategory::UnsupportedFeature},
			FCase{"MorphTargets.gltf", ESceneImportDiagnosticCategory::UnsupportedFeature},
			FCase{"InvalidJointIndex.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"ZeroWeights.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"NaNWeights.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"NonFiniteInverseBind.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"UnorderedKeyTimes.gltf", ESceneImportDiagnosticCategory::MalformedSource},
			FCase{"DuplicateKeyTimes.gltf", ESceneImportDiagnosticCategory::MalformedSource}};
		for (const FCase& Case : Cases)
		{
			SCOPED_TRACE(Case.File);
			FImportedSceneData Scene;
			EXPECT_FALSE(ImportFromFile(
				TestDataPath(std::format("Skeletal/Malformed/{}", Case.File)), Scene));
			EXPECT_TRUE(Scene.Skeletons.empty());
			EXPECT_TRUE(Scene.SkeletalMeshes.empty());
			EXPECT_TRUE(Scene.AnimationClips.empty());
			EXPECT_TRUE(std::ranges::any_of(Scene.Diagnostics, [&](const FSceneImportDiagnostic& Diagnostic) {
				return Diagnostic.Severity == EImportDiagnosticSeverity::Error
					&& Diagnostic.Category == Case.Category;
			}));
		}
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
			{"Filtered.gltf", {"Unused", "Red", "Blue"}, {1, 2}, {{"Red", 1}, {"Blue", 2}}},
			{"UnusedDuplicate.gltf", {"Shared", "Shared", "Red"}, {1, 2}, {{"Shared", 1}, {"Red", 2}}},
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

	TEST(FAssetImportTests, SceneMaterialContractFixturesRemainImportable)
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

	TEST(FAssetImportTests, GltfPrimitiveProjectionPreservesMaterialsAcrossNodeInstances)
	{
		FImportedSceneData Scene;
		ASSERT_TRUE(ImportFromFile(
			TestDataPath("StaticModelMaterials/PrimitiveProjection.gltf"), Scene));

		ASSERT_EQ(Scene.Meshes.size(), 4u);
		const std::array<uint32, 4> ExpectedInstanceMaterials = {1, 2, 0, 1};
		for (size_t Index = 0; Index < ExpectedInstanceMaterials.size(); ++Index)
		{
			EXPECT_EQ(Scene.Meshes[Index].SourceMaterialIndex, ExpectedInstanceMaterials[Index]);
		}

		ASSERT_EQ(Scene.MaterialSlots.size(), 3u);
		EXPECT_EQ(Scene.MaterialSlots[0].Name, "Zero");
		EXPECT_EQ(Scene.MaterialSlots[0].SourceMaterialIndex, 0u);
		EXPECT_EQ(Scene.MaterialSlots[1].Name, "One");
		EXPECT_EQ(Scene.MaterialSlots[1].SourceMaterialIndex, 1u);
		EXPECT_EQ(Scene.MaterialSlots[2].Name, "Two");
		EXPECT_EQ(Scene.MaterialSlots[2].SourceMaterialIndex, 2u);
		EXPECT_TRUE(std::ranges::none_of(
			Scene.MaterialSlots,
			[](const FImportedMaterialSlot& Slot) { return Slot.SourceName == "Unused"; }));
	}

	TEST(FAssetImportTests, SceneGoldenSnapshotFreezesRequiredAndOptionalCases)
	{
		EXPECT_EQ(ImportedSceneParserVersion, 3u);
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
			Fixtures.GetView("PrimitiveProjection.gltf").GetView("sourcePrimitiveMaterials").Num(),
			3u);
		EXPECT_EQ(
			Fixtures.GetView("PrimitiveProjection.gltf").GetView("requiredInstancedMeshMaterials").Num(),
			4u);
		EXPECT_EQ(
			Fixtures.GetView("PrimitiveProjection.gltf").GetView("importedMeshMaterials").Num(),
			4u);
		EXPECT_EQ(
			Fixtures.GetView("EmbeddedImage.glb").GetView("images").GetView(0).GetView("identity").GetString(),
			"glb-buffer-view:4");
	}

	TEST(FAssetImportTests, ImportsFrozenGltfMaterialImageAndDependencyContract)
	{
		FImportedSceneData Scene;
		FMeshImportOptions Options;
		Options.RootSource.Path = "/Game/Models/MaterialContract.gltf";
		ASSERT_TRUE(ImportFromFile(
			TestDataPath("StaticModelMaterials/MaterialContract.gltf"), Scene, Options));

		ASSERT_EQ(Scene.Dependencies.size(), 3u);
		EXPECT_EQ(Scene.Dependencies[0].Role, EImportedDependencyRole::RootScene);
		EXPECT_EQ(Scene.Dependencies[0].StableIdentity, "root");
		EXPECT_EQ(Scene.Dependencies[0].Source.Path, "/Game/Models/MaterialContract.gltf");
		EXPECT_EQ(Scene.Dependencies[1].Role, EImportedDependencyRole::GeometryBuffer);
		EXPECT_EQ(Scene.Dependencies[1].StableIdentity, "buffer:Triangle.bin");
		EXPECT_EQ(Scene.Dependencies[1].Source.Path, "/Game/Models/Triangle.bin");
		EXPECT_EQ(Scene.Dependencies[2].Role, EImportedDependencyRole::Image);
		EXPECT_EQ(Scene.Dependencies[2].StableIdentity, "image:Red.png");
		EXPECT_EQ(Scene.Dependencies[2].Source.Path, "/Game/Models/Red.png");
		for (const FImportedDependency& Dependency : Scene.Dependencies)
		{
			EXPECT_GT(Dependency.ByteCount, 0u);
			EXPECT_FALSE(Dependency.ContentHash.IsZero());
		}

		ASSERT_EQ(Scene.Images.size(), 1u);
		EXPECT_EQ(Scene.Images[0].StableIdentity, "external:Red.png");
		EXPECT_EQ(Scene.Images[0].SuggestedName, "RedPixel");
		EXPECT_EQ(Scene.Images[0].Encoding, EImportedImageEncoding::Png);
		EXPECT_EQ(Scene.Images[0].EncodedByteCount, 68u);
		ASSERT_TRUE(Scene.Images[0].ExternalDependencyIndex.has_value());
		EXPECT_EQ(*Scene.Images[0].ExternalDependencyIndex, 2u);
		EXPECT_TRUE(Scene.Images[0].EmbeddedEncodedBytes.empty());

		ASSERT_EQ(Scene.Materials.size(), 4u);
		const FImportedMaterial& Shared = Scene.Materials[0];
		EXPECT_EQ(Shared.SourceMaterialIndex, 0u);
		EXPECT_EQ(Shared.SourceName, "Shared");
		ExpectVec4Eq({0.8f, 0.6f, 0.4f, 0.5f}, Shared.BaseColorFactor);
		EXPECT_FLOAT_EQ(Shared.MetallicFactor, 0.25f);
		EXPECT_FLOAT_EQ(Shared.RoughnessFactor, 0.75f);
		EXPECT_TRUE(Shared.bDoubleSided);
		ASSERT_EQ(Shared.TextureBindings.size(), 1u);
		const FImportedTextureBinding& SharedColor = Shared.TextureBindings[0];
		EXPECT_EQ(SharedColor.Semantic, EImportedTextureSemantic::BaseColor);
		EXPECT_EQ(SharedColor.ImageIndex, 0u);
		EXPECT_EQ(SharedColor.UVChannel, 1u);
		ExpectVec2Eq({0.1f, 0.2f}, SharedColor.Offset);
		ExpectVec2Eq({2.0f, 3.0f}, SharedColor.Scale);
		EXPECT_EQ(SharedColor.Sampler.MinFilter, EImportedSamplerFilter::LinearMipmapLinear);
		EXPECT_EQ(SharedColor.Sampler.MagFilter, EImportedSamplerFilter::Linear);
		EXPECT_EQ(SharedColor.Sampler.WrapU, EImportedSamplerWrap::Repeat);
		EXPECT_EQ(SharedColor.Sampler.WrapV, EImportedSamplerWrap::Repeat);

		const FImportedMaterial& Masked = Scene.Materials[1];
		EXPECT_EQ(Masked.SourceName, "Shared");
		EXPECT_EQ(Masked.AlphaMode, EImportedAlphaMode::Mask);
		EXPECT_FLOAT_EQ(Masked.AlphaCutoff, 0.33f);
		ASSERT_EQ(Masked.TextureBindings.size(), 1u);
		EXPECT_EQ(Masked.TextureBindings[0].Sampler.MinFilter, EImportedSamplerFilter::Nearest);
		EXPECT_EQ(Masked.TextureBindings[0].Sampler.MagFilter, EImportedSamplerFilter::Nearest);
		EXPECT_EQ(Masked.TextureBindings[0].Sampler.WrapU, EImportedSamplerWrap::ClampToEdge);
		EXPECT_EQ(Masked.TextureBindings[0].Sampler.WrapV, EImportedSamplerWrap::MirroredRepeat);

		EXPECT_EQ(Scene.Materials[2].AlphaMode, EImportedAlphaMode::Blend);
		EXPECT_EQ(Scene.Materials[3].SourceName, "Unused");
		ASSERT_EQ(Scene.MaterialSlots.size(), 3u);
		EXPECT_EQ(Scene.MaterialSlots[0].Name, "Shared");
		EXPECT_EQ(Scene.MaterialSlots[1].Name, "Shared_1");
		EXPECT_EQ(Scene.MaterialSlots[2].Name, "Blend");
		EXPECT_TRUE(HasDiagnostic(Scene, EImportDiagnosticSeverity::Warning,
			ESceneImportDiagnosticCategory::UnsupportedOptionalExtension, "EXT_fixture_optional"));
	}

	TEST(FAssetImportTests, ImportsFrozenEmbeddedImageForms)
	{
		for (const auto& [FileName, Identity, SuggestedName] : {
			std::tuple{"StaticModelMaterials/DataUriImage.gltf", "data-uri:0", "InlinePixel"},
			std::tuple{"StaticModelMaterials/EmbeddedImage.glb", "glb-buffer-view:4", "EmbeddedPixel"}})
		{
			SCOPED_TRACE(FileName);
			FImportedSceneData Scene;
			ASSERT_TRUE(ImportFromFile(TestDataPath(FileName), Scene));
			ASSERT_EQ(Scene.Images.size(), 1u);
			EXPECT_EQ(Scene.Images[0].StableIdentity, Identity);
			EXPECT_EQ(Scene.Images[0].SuggestedName, SuggestedName);
			EXPECT_EQ(Scene.Images[0].Encoding, EImportedImageEncoding::Png);
			EXPECT_FALSE(Scene.Images[0].ExternalDependencyIndex.has_value());
			EXPECT_EQ(Scene.Images[0].EmbeddedEncodedBytes.size(), Scene.Images[0].EncodedByteCount);
			ASSERT_EQ(Scene.Dependencies.size(), 1u);
			EXPECT_EQ(Scene.Dependencies[0].Role, EImportedDependencyRole::RootScene);
		}
	}

	TEST(FAssetImportTests, RejectsRequiredExtensionAndMissingDccDependencyWithoutPartialOutput)
	{
		struct FCase
		{
			std::string FileName;
			ESceneImportDiagnosticCategory Category;
			std::string Subject;
		};
		for (const FCase& Case : {
			FCase{"StaticModelMaterials/RequiredExtension.gltf",
				ESceneImportDiagnosticCategory::UnsupportedRequiredExtension, "EXT_fixture_required"},
			FCase{"StaticModelMaterials/UnsupportedDccMaterial.fbx",
				ESceneImportDiagnosticCategory::MissingDependency, "Textures/albedo.png"}})
		{
			SCOPED_TRACE(Case.FileName);
			FImportedSceneData Scene;
			EXPECT_FALSE(ImportFromFile(TestDataPath(Case.FileName), Scene));
			EXPECT_TRUE(Scene.Images.empty());
			EXPECT_TRUE(Scene.Materials.empty());
			EXPECT_TRUE(Scene.MaterialSlots.empty());
			EXPECT_TRUE(Scene.Meshes.empty());
			EXPECT_TRUE(HasDiagnostic(Scene, EImportDiagnosticSeverity::Error, Case.Category, Case.Subject));
		}
	}

	TEST(FAssetImportTests, ReportsStructuredFallbackForUnsupportedFbxShading)
	{
		FImportedSceneData Scene;
		EXPECT_FALSE(ImportFromFile(
			TestDataPath("StaticModelMaterials/UnsupportedDccMaterial.fbx"), Scene));
		EXPECT_TRUE(HasDiagnostic(
			Scene,
			EImportDiagnosticSeverity::Warning,
			ESceneImportDiagnosticCategory::UnsupportedMaterialProperty,
			"unmapped-material-properties"));
		EXPECT_TRUE(HasDiagnostic(
			Scene,
			EImportDiagnosticSeverity::Error,
			ESceneImportDiagnosticCategory::MissingDependency,
			"Textures/albedo.png"));
	}

	TEST(FAssetImportTests, MapsFrozenFbxDiffuseOpacitySubset)
	{
		FImportedSceneData Scene;
		ASSERT_TRUE(ImportFromFile(TestDataPath("StaticModelMaterials/PhongMaterial.fbx"), Scene));
		ASSERT_FALSE(Scene.Materials.empty());
		const auto It = std::ranges::find(Scene.Materials, std::string("phong1"), &FImportedMaterial::SourceName);
		ASSERT_NE(It, Scene.Materials.end());
		ExpectVec4Eq({0.5f, 0.25f, 0.25f, 0.5f}, It->BaseColorFactor);
		EXPECT_TRUE(HasDiagnostic(Scene, EImportDiagnosticSeverity::Warning,
			ESceneImportDiagnosticCategory::UnsupportedMaterialProperty, "Phong"));
	}

	TEST(FAssetImportTests, RejectsMalformedReferenceAndMaterialBudgetBeforeAssimpPublication)
	{
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "NormalizedImportFailures";
		std::filesystem::create_directories(Root);

		const std::filesystem::path InvalidReference = Root / "InvalidReference.gltf";
		{
			std::ofstream Stream(InvalidReference, std::ios::trunc);
			Stream << R"({
				"asset":{"version":"2.0"},
				"images":[],
				"textures":[{"source":12}],
				"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}]
			})";
		}
		FImportedSceneData InvalidScene;
		EXPECT_FALSE(ImportFromFile(InvalidReference.generic_string(), InvalidScene));
		EXPECT_TRUE(InvalidScene.Images.empty());
		EXPECT_TRUE(InvalidScene.Materials.empty());
		EXPECT_TRUE(HasDiagnostic(InvalidScene, EImportDiagnosticSeverity::Error,
			ESceneImportDiagnosticCategory::InvalidReference, "material:0:baseColorTexture"));

		const std::filesystem::path OverBudget = Root / "OverBudget.gltf";
		{
			std::ofstream Stream(OverBudget, std::ios::trunc);
			Stream << R"({"asset":{"version":"2.0"},"materials":[)";
			for (uint32 Index = 0; Index <= MaxImportedSourceMaterials; ++Index)
			{
				if (Index != 0) Stream << ',';
				Stream << "{}";
			}
			Stream << "]}";
		}
		FImportedSceneData OverBudgetScene;
		EXPECT_FALSE(ImportFromFile(OverBudget.generic_string(), OverBudgetScene));
		EXPECT_TRUE(OverBudgetScene.Materials.empty());
		EXPECT_TRUE(HasDiagnostic(OverBudgetScene, EImportDiagnosticSeverity::Error,
			ESceneImportDiagnosticCategory::ResourceLimitExceeded, "materials"));
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

		ExpectVec3Eq(FVector3f(0.0f, 0.0f, 0.0f), Mesh.Positions[0]);
		ExpectVec3Eq(FVector3f(0.0f, 1.0f, 0.0f), Mesh.Positions[1]);
		ExpectVec3Eq(FVector3f(0.0f, 0.0f, 1.0f), Mesh.Positions[2]);
		for (size_t VertexIndex = 0; VertexIndex < Mesh.Positions.size(); ++VertexIndex)
		{
			ExpectVec3Eq(FVector3f(-1.0f, 0.0f, 0.0f), Mesh.Normals[VertexIndex]);
			ExpectVec3Eq(FVector3f(0.0f, 1.0f, 0.0f), FVector3f(Mesh.Tangents[VertexIndex]));
			EXPECT_FLOAT_EQ(Mesh.Tangents[VertexIndex].w, -1.0f);
		}
		EXPECT_EQ(Mesh.Indices, (std::vector<uint32>{0, 2, 1}));
	}

}
