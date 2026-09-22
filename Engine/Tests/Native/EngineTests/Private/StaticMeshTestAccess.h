#pragma once

#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshCompilation.h"

namespace Durin
{
	// Gives focused tests mutable construction access without exposing installed
	// render-data mutation through the runtime asset API.
	class FStaticMeshTestAccess
	{
	public:
		static auto SetMaterialSlots(DStaticMesh* Mesh, std::vector<FMeshMaterialSlotDefinition> Slots) -> void
		{ Mesh->MaterialSlots = std::move(Slots); }
		static auto MakeSlots(const FStaticMeshDecodedGeometry& Geometry) -> std::vector<FMeshMaterialSlotDefinition>
		{
			std::vector<FMeshMaterialSlotDefinition> Slots;
			for (const auto& Slot : Geometry.MaterialSlots)
				Slots.push_back({.Name = FName(Slot.Name), .SourceName = Slot.SourceName,
					.SourceMaterialIndex = Slot.SourceMaterialIndex});
			return Slots;
		}
		static auto MakeSlots(const FStaticMeshSource& Source) -> std::vector<FMeshMaterialSlotDefinition>
		{
			const auto Geometry = Source.AcquireGeometry();
			return Geometry ? MakeSlots(**Geometry) : std::vector<FMeshMaterialSlotDefinition>{};
		}
		static auto Build(DStaticMesh* Mesh, FStaticMeshDecodedGeometry Geometry)
		{
			auto Slots = MakeSlots(Geometry);
			return Mesh->Build(std::move(Geometry), std::move(Slots));
		}
		static auto Build(DStaticMesh* Mesh, const FStaticMeshSource& Source)
		{
			return Mesh->Build(Source);
		}

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
