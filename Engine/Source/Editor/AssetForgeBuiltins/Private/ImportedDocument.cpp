#include "AssetForge/Builtins/ImportedDocument.h"
#include "ImportedSceneInternal.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto MakeMaterialSlots(const FImportedDocument& Document, FImportedSceneData& Geometry,
			std::string& Error) -> bool
		{
			std::unordered_set<uint32> Used;
			for (const auto& Mesh : Geometry.Meshes) Used.insert(Mesh.SourceMaterialIndex);
			std::unordered_map<std::string, uint32> Names;
			for (const auto& Material : Document.Materials)
				if (Used.erase(Material.SourceMaterialIndex))
					Geometry.MaterialSlots.push_back({
						Private::MakeUniqueName(Material.SourceName, Material.SourceMaterialIndex, Names),
						Material.SourceMaterialIndex, Material.SourceName});
			if (!Used.empty())
			{
				Error = "Mesh resource references a missing source material.";
				return false;
			}
			return true;
		}

		auto NormalizeSafe(const FVector3f& Value) -> FVector3f
		{
			const auto LengthSquared = glm::dot(Value, Value);
			return LengthSquared > 0 && std::isfinite(LengthSquared)
				? Value / std::sqrt(LengthSquared) : FVector3f(0);
		}

		auto TransformPrimitive(FImportedMeshData& Mesh, const FMatrix4f& Transform,
			std::string& Error) -> bool
		{
			const glm::mat3 Linear(Transform);
			const float Determinant = glm::determinant(Linear);
			if (!std::isfinite(Determinant) || std::abs(Determinant) <= 1.0e-8f)
			{
				Error = "Cannot expand a mesh under a singular or nonfinite transform.";
				return false;
			}
			const auto NormalMatrix = glm::transpose(glm::inverse(Linear));
			for (auto& Position : Mesh.Positions)
			{
				Position = FVector3f(Transform * FVector4f(Position, 1));
				if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z))
				{
					Error = "Combined mesh expansion produced a nonfinite position.";
					return false;
				}
			}
			for (auto& Normal : Mesh.Normals) Normal = NormalizeSafe(NormalMatrix * Normal);
			for (size_t Index = 0; Index < Mesh.Tangents.size(); ++Index)
			{
				auto& Tangent = Mesh.Tangents[Index];
				auto Direction = Linear * FVector3f(Tangent);
				if (Index < Mesh.Normals.size())
					Direction -= Mesh.Normals[Index] * glm::dot(Mesh.Normals[Index], Direction);
				Direction = NormalizeSafe(Direction);
				Tangent = FVector4f(Direction, Tangent.w * (Determinant < 0 ? -1.0f : 1.0f));
			}
			if (Mesh.Indices.size() % 3 != 0)
			{
				Error = "Source primitive does not contain triangle indices.";
				return false;
			}
			if (Determinant < 0)
				for (size_t Index = 0; Index < Mesh.Indices.size(); Index += 3)
					std::swap(Mesh.Indices[Index + 1], Mesh.Indices[Index + 2]);
			return true;
		}
	}

	auto SelectImportedMeshResource(const FImportedDocument& Document, uint32 MeshIndex,
		FImportedSceneData& OutGeometry, std::string& OutError) -> bool
	{
		OutGeometry = {};
		if (MeshIndex >= Document.Meshes.size())
		{
			OutError = "Requested mesh resource is outside the source table.";
			return false;
		}
		FImportedSceneData Geometry;
		Geometry.Meshes = Document.Meshes[MeshIndex].Primitives;
		if (!MakeMaterialSlots(Document, Geometry, OutError)) return false;
		OutGeometry = std::move(Geometry);
		return true;
	}

	auto ExpandImportedDocument(const FImportedDocument& Document, FImportedSceneData& OutGeometry,
		std::string& OutError, std::span<const uint32> SceneIndices, std::span<const uint32> MeshIndices) -> bool
	{
		OutGeometry = {};
		FImportedSceneData Geometry;
		if (SceneIndices.empty() && !Document.Scenes.empty())
			SceneIndices = std::span{&Document.DefaultSceneIndex, size_t(1)};
		std::vector<uint32> Pending;
		for (const auto SceneIndex : SceneIndices)
		{
			if (SceneIndex >= Document.Scenes.size())
			{
				OutError = "Requested source scene is outside the source table.";
				return false;
			}
			const auto& Roots = Document.Scenes[SceneIndex].RootNodeIndices;
			Pending.insert(Pending.end(), Roots.rbegin(), Roots.rend());
		}
		std::vector<std::vector<uint32>> Children(Document.Nodes.size());
		for (uint32 Index = 0; Index < Document.Nodes.size(); ++Index)
		{
			const auto Parent = Document.Nodes[Index].ParentNodeIndex;
			if (Parent < -1 || Parent >= static_cast<int32>(Document.Nodes.size()))
			{
				OutError = "Source node has an invalid parent.";
				return false;
			}
			if (Parent >= 0) Children[Parent].push_back(Index);
		}
		std::vector<bool> Seen(Document.Nodes.size());
		while (!Pending.empty())
		{
			if (Private::IsSceneImportCancellationRequested())
			{
				OutError = "Mesh instance expansion canceled.";
				return false;
			}
			const auto Index = Pending.back(); Pending.pop_back();
			if (Index >= Document.Nodes.size())
			{
				OutError = "Source scene references an invalid node.";
				return false;
			}
			if (Seen[Index]) continue; // Shared roots across selected scenes expand once.
			Seen[Index] = true;
			const auto& Node = Document.Nodes[Index];
			for (const auto MeshIndex : Node.MeshIndices)
			{
				if (MeshIndex >= Document.Meshes.size())
				{
					OutError = "Source node references an invalid mesh resource.";
					return false;
				}
				if (!MeshIndices.empty() && !std::ranges::contains(MeshIndices, MeshIndex)) continue;
				for (const auto& Primitive : Document.Meshes[MeshIndex].Primitives)
				{
					auto Mesh = Primitive;
					if (!TransformPrimitive(Mesh, Node.GlobalTransform, OutError)) return false;
					Geometry.Meshes.push_back(std::move(Mesh));
				}
			}
			Pending.insert(Pending.end(), Children[Index].rbegin(), Children[Index].rend());
		}
		if (!MakeMaterialSlots(Document, Geometry, OutError)) return false;
		OutGeometry = std::move(Geometry);
		return true;
	}
}
