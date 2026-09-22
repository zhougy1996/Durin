#pragma once

#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	// Gives focused tests mutable construction access without exposing installed
	// render-data mutation through the runtime asset API.
	class FStaticMeshTestAccess
	{
	public:
		static auto ReplaceRenderData(DStaticMesh* Mesh, std::unique_ptr<FStaticMeshRenderData> Data,
			std::vector<FMeshMaterialSlotDefinition> Slots) -> std::expected<void, FStaticMeshReplacementError>
		{ return Mesh->ReplaceRenderDataDestructively(std::move(Data), std::move(Slots)); }
		static auto ReplaceSourceRenderData(DStaticMesh* Mesh, FStaticMeshSource Source,
			std::unique_ptr<FStaticMeshRenderData> Data, std::vector<FMeshMaterialSlotDefinition> Slots,
			float NormalizedSize) -> std::expected<void, FStaticMeshReplacementError>
		{ return Mesh->ReplaceSourceRenderDataDestructively(std::move(Source), std::move(Data), std::move(Slots), NormalizedSize); }
		static auto GetRenderDataUpdateError(const DStaticMesh* Mesh) -> const FStaticMeshReplacementError&
		{ return Mesh->GetRenderDataUpdateError(); }
		static auto GetMutableRenderData(DStaticMesh* Mesh)
			-> FStaticMeshRenderData*
		{
			return const_cast<FStaticMeshRenderData*>(
				Mesh ? Mesh->GetRenderData() : nullptr);
		}
	};
}
