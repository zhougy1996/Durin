#include "ImportedSceneInternal.h"

#include "Logging/LogMacros.h"

#include <assimp/matrix3x3.h>
#include <assimp/scene.h>

namespace Durin::AssetForge::Builtins::Private
{
	using namespace Durin;
	constexpr float TransformDeterminantTolerance = 1.0e-8f;

	auto ToAssimpMatrix(const FMatrix4f& Matrix) -> aiMatrix4x4
	{
		// GLM stores columns while Assimp's constructor is expressed as rows.
		return {
			Matrix[0][0], Matrix[1][0], Matrix[2][0], Matrix[3][0],
			Matrix[0][1], Matrix[1][1], Matrix[2][1], Matrix[3][1],
			Matrix[0][2], Matrix[1][2], Matrix[2][2], Matrix[3][2],
			Matrix[0][3], Matrix[1][3], Matrix[2][3], Matrix[3][3]
		};
	}

	auto ToVector3(const aiVector3D& Value) -> FVector3f
	{
		return {Value.x, Value.y, Value.z};
	}

	auto IsFinite(const aiVector3D& Value) -> bool
	{
		return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
	}

	auto ImportMeshInstance(
		const aiScene& Scene,
		const aiNode& Node,
		uint32 MeshIndex,
		std::span<const uint32> SourceMaterialIndices,
		const aiMatrix4x4& Transform,
		FImportedSceneData& OutScene,
		std::string& OutError) -> bool
	{
		if (MeshIndex >= Scene.mNumMeshes || Scene.mMeshes[MeshIndex] == nullptr)
		{
			OutError = std::format("Scene node '{}' references invalid mesh index {}.", Node.mName.C_Str(), MeshIndex);
			return false;
		}

		const aiMesh& Mesh = *Scene.mMeshes[MeshIndex];
		const aiMatrix3x3 LinearTransform(Transform);
		const float Determinant = LinearTransform.Determinant();
		if (!std::isfinite(Determinant) || std::abs(Determinant) <= TransformDeterminantTolerance)
		{
			OutError = std::format("Mesh '{}' is under a singular node transform.", Mesh.mName.C_Str());
			return false;
		}

		aiMatrix3x3 NormalTransform(LinearTransform);
		NormalTransform.Inverse().Transpose();
		const bool bMirrored = Determinant < 0.0f;

		FImportedMeshData OutMesh;
		OutMesh.Name = Mesh.mName.length > 0 ? Mesh.mName.C_Str() : Node.mName.C_Str();
		if (OutMesh.Name.empty()) OutMesh.Name = std::format("Mesh_{}", MeshIndex);
		OutMesh.SourceMaterialIndex = SourceMaterialIndices.empty()
			? Mesh.mMaterialIndex
			: SourceMaterialIndices[MeshIndex];
		if (Mesh.GetNumUVChannels() > MaxImportedUVChannels)
		{
			DURIN_WARN("Mesh '{}' has {} UV channels; only the first {} are imported.", OutMesh.Name, Mesh.GetNumUVChannels(), MaxImportedUVChannels);
		}
		OutMesh.Positions.reserve(Mesh.mNumVertices);
		if (Mesh.HasNormals()) OutMesh.Normals.reserve(Mesh.mNumVertices);
		if (Mesh.HasTangentsAndBitangents()) OutMesh.Tangents.reserve(Mesh.mNumVertices);
		if (Mesh.HasVertexColors(0)) OutMesh.Colors.reserve(Mesh.mNumVertices);
		for (uint32 Channel = 0; Channel < MaxImportedUVChannels; ++Channel)
		{
			if (Mesh.HasTextureCoords(Channel)) OutMesh.UVChannels[Channel].reserve(Mesh.mNumVertices);
		}

		for (unsigned int VertexIndex = 0; VertexIndex < Mesh.mNumVertices; ++VertexIndex)
		{
			if ((VertexIndex & 0xfffu) == 0
				&& Private::IsSceneImportCancellationRequested())
			{
				OutError = "Scene geometry decoding was canceled.";
				return false;
			}
			const aiVector3D Position = Transform * Mesh.mVertices[VertexIndex];
			if (!IsFinite(Position))
			{
				OutError = std::format("Mesh '{}' produced a non-finite transformed position.", OutMesh.Name);
				return false;
			}
			OutMesh.Positions.emplace_back(ToVector3(Position));

			if (Mesh.HasNormals())
			{
				aiVector3D Normal = NormalTransform * Mesh.mNormals[VertexIndex];
				Normal.NormalizeSafe();
				if (IsFinite(Normal)) OutMesh.Normals.emplace_back(ToVector3(Normal));
			}

			if (Mesh.HasTangentsAndBitangents())
			{
				aiVector3D Tangent = LinearTransform * Mesh.mTangents[VertexIndex];
				aiVector3D Bitangent = LinearTransform * Mesh.mBitangents[VertexIndex];
				aiVector3D Normal = Mesh.HasNormals() ? NormalTransform * Mesh.mNormals[VertexIndex] : aiVector3D(0.0f, 0.0f, 1.0f);
				Normal.NormalizeSafe();
				Tangent -= Normal * (Normal * Tangent);
				Tangent.NormalizeSafe();
				Bitangent.NormalizeSafe();
				if (IsFinite(Tangent) && IsFinite(Bitangent) && IsFinite(Normal))
				{
					const float Sign = ((Normal ^ Tangent) * Bitangent) < 0.0f ? -1.0f : 1.0f;
					OutMesh.Tangents.emplace_back(Tangent.x, Tangent.y, Tangent.z, Sign);
				}
			}

			if (Mesh.HasVertexColors(0))
			{
				const aiColor4D& Color = Mesh.mColors[0][VertexIndex];
				OutMesh.Colors.emplace_back(Color.r, Color.g, Color.b, Color.a);
			}

			for (uint32 Channel = 0; Channel < MaxImportedUVChannels; ++Channel)
			{
				if (Mesh.HasTextureCoords(Channel))
				{
					const aiVector3D& UV = Mesh.mTextureCoords[Channel][VertexIndex];
					OutMesh.UVChannels[Channel].emplace_back(UV.x, UV.y);
				}
			}
		}

		if (Mesh.mNumFaces > std::numeric_limits<uint32>::max() / 3u)
		{
			OutError = std::format("Mesh '{}' exceeds the uint32 index limit.", OutMesh.Name);
			return false;
		}
		OutMesh.Indices.reserve(Mesh.mNumFaces * 3u);
		for (unsigned int FaceIndex = 0; FaceIndex < Mesh.mNumFaces; ++FaceIndex)
		{
			if ((FaceIndex & 0xfffu) == 0
				&& Private::IsSceneImportCancellationRequested())
			{
				OutError = "Scene geometry decoding was canceled.";
				return false;
			}
			const aiFace& Face = Mesh.mFaces[FaceIndex];
			if (Face.mNumIndices != 3)
			{
				OutError = std::format("Mesh '{}' contains a non-triangle face after triangulation.", OutMesh.Name);
				return false;
			}
			const uint32 I0 = Face.mIndices[0];
			const uint32 I1 = Face.mIndices[bMirrored ? 2 : 1];
			const uint32 I2 = Face.mIndices[bMirrored ? 1 : 2];
			if (I0 >= Mesh.mNumVertices || I1 >= Mesh.mNumVertices || I2 >= Mesh.mNumVertices)
			{
				OutError = std::format("Mesh '{}' contains an out-of-range index.", OutMesh.Name);
				return false;
			}
			OutMesh.Indices.insert(OutMesh.Indices.end(), {I0, I1, I2});
		}

		OutScene.Meshes.emplace_back(std::move(OutMesh));
		return true;
	}

	auto ImportNodeMeshes(
		const aiScene& Scene,
		const aiNode& Node,
		std::span<const uint32> SourceMaterialIndices,
		const aiMatrix4x4& ParentTransform,
		FImportedSceneData& OutScene,
		std::string& OutError) -> bool
	{
		if (Private::IsSceneImportCancellationRequested())
		{
			OutError = "Scene geometry decoding was canceled.";
			return false;
		}
		const aiMatrix4x4 Transform = ParentTransform * Node.mTransformation;
		for (unsigned int MeshReferenceIndex = 0; MeshReferenceIndex < Node.mNumMeshes; ++MeshReferenceIndex)
		{
			if (!ImportMeshInstance(Scene, Node, Node.mMeshes[MeshReferenceIndex],
				SourceMaterialIndices, Transform, OutScene, OutError)) return false;
		}
		for (unsigned int ChildIndex = 0; ChildIndex < Node.mNumChildren; ++ChildIndex)
		{
			if (Node.mChildren[ChildIndex] == nullptr) continue;
			if (!ImportNodeMeshes(Scene, *Node.mChildren[ChildIndex],
				SourceMaterialIndices, Transform, OutScene, OutError)) return false;
		}
		return true;
	}

	auto ImportAssimpGeometry(
		const aiScene& Scene,
		const FMeshImportOptions& Options,
		std::span<const uint32> SourceMaterialIndices,
		FImportedSceneData& OutScene,
		std::string& OutError) -> bool
	{
		return ImportNodeMeshes(
			Scene,
			*Scene.mRootNode,
			SourceMaterialIndices,
			ToAssimpMatrix(Options.SourceToEngine),
			OutScene,
			OutError);
	}
	auto ImportAssimpDocumentGeometry(const aiScene& Scene,
		const FMeshImportOptions& Options, bool bGltf,
		FImportedDocument& Document, std::string& OutError) -> bool
	{
		if (Scene.mNumMeshes > MaxImportedSourceMeshes)
		{
			OutError = "Decoded mesh resource limit exceeded.";
			return false;
		}
		const auto Basis = ToAssimpMatrix(Options.SourceToEngine);
		std::unordered_map<std::string, FImportedMeshData*> PrimitiveByToken;
		if (bGltf)
			for (auto& Resource : Document.Meshes)
				for (auto& Primitive : Resource.Primitives)
					PrimitiveByToken.emplace(Primitive.Name, &Primitive);
		else Document.Meshes.resize(Scene.mNumMeshes);
		for (uint32 Index = 0; Index < Scene.mNumMeshes; ++Index)
		{
			FImportedSceneData Decoded;
			if (!ImportMeshInstance(Scene, *Scene.mRootNode, Index, {}, Basis, Decoded, OutError)) return false;
			auto Primitive = std::move(Decoded.Meshes.front());
			if (bGltf)
			{
				const auto Found = PrimitiveByToken.find(Primitive.Name);
				if (Found == PrimitiveByToken.end())
				{
					OutError = "Decoder returned an unknown or duplicated glTF primitive token.";
					return false;
				}
				Primitive.SourceMaterialIndex = Found->second->SourceMaterialIndex;
				*Found->second = std::move(Primitive);
				PrimitiveByToken.erase(Found);
			}
			else
			{
				auto& Resource = Document.Meshes[Index];
				Resource.SourceMeshIndex = Index;
				Resource.SourceName = Primitive.Name;
				Resource.Primitives.push_back(std::move(Primitive));
			}
		}
		if (bGltf && !PrimitiveByToken.empty())
		{
			OutError = "Decoder omitted a glTF source primitive.";
			return false;
		}
		for (const auto& Resource : Document.Meshes)
			for (const auto& Primitive : Resource.Primitives)
				if (std::ranges::none_of(Document.Materials, [&](const auto& Material) {
					return Material.SourceMaterialIndex == Primitive.SourceMaterialIndex;
				}))
				{
					OutError = "Source primitive references a missing material.";
					return false;
				}
		if (bGltf) return true;

		auto ToMatrix = [](const aiMatrix4x4& M) {
			FMatrix4f Result(1);
			for (uint32 Row = 0; Row < 4; ++Row)
				for (uint32 Column = 0; Column < 4; ++Column) Result[Column][Row] = M[Row][Column];
			return Result;
		};
		const auto InverseBasis = glm::inverse(Options.SourceToEngine);
		std::vector<std::pair<const aiNode*, int32>> Pending{{Scene.mRootNode, -1}};
		while (!Pending.empty())
		{
			if (IsSceneImportCancellationRequested() || Document.Nodes.size() >= MaxImportedSceneNodes)
			{
				OutError = "Source node decoding canceled or exceeded its resource limit.";
				return false;
			}
			const auto [Source, Parent] = Pending.back(); Pending.pop_back();
			const auto Index = static_cast<uint32>(Document.Nodes.size());
			auto& Node = Document.Nodes.emplace_back();
			Node.SourceNodeIndex = Index;
			Node.ParentNodeIndex = Parent;
			Node.SourceName = Source->mName.C_Str();
			Node.LocalTransform = Options.SourceToEngine * ToMatrix(Source->mTransformation) * InverseBasis;
			Node.GlobalTransform = Parent < 0 ? Node.LocalTransform
				: Document.Nodes[Parent].GlobalTransform * Node.LocalTransform;
			for (uint32 Mesh = 0; Mesh < Source->mNumMeshes; ++Mesh)
			{
				if (Source->mMeshes[Mesh] >= Document.Meshes.size())
				{
					OutError = "Source node references a missing mesh resource.";
					return false;
				}
				Node.MeshIndices.push_back(Source->mMeshes[Mesh]);
			}
			for (uint32 Child = Source->mNumChildren; Child > 0; --Child)
				if (Source->mChildren[Child - 1]) Pending.emplace_back(Source->mChildren[Child - 1], Index);
		}
		Document.Scenes.push_back({.SourceName = Scene.mRootNode->mName.C_Str(), .RootNodeIndices = {0}});
		return true;
	}

}
