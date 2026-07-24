#pragma once

#include "EngineAPI.h"
#include "Math/Box.h"

#include "RHIResources.h"

namespace Durin
{
	inline constexpr uint32 MaxStaticMeshUVChannels = 4;

	struct FStaticMeshMaterialSlot
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
		FGuid SlotId;
	};

	struct FStaticMeshSection
	{
		std::string Name;
		uint32 FirstIndex = 0;
		uint32 IndexCount = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		uint32 MaterialSlotIndex = 0;
		FBox LocalBounds;
	};

	struct FStaticMeshPackedVertex
	{
		std::array<int16, 4> Normal{};
		std::array<int16, 4> Tangent{};
		std::array<FVector2f, MaxStaticMeshUVChannels> TexCoords{};
		std::array<uint8, 4> Color{};
	};

	static_assert(sizeof(FStaticMeshPackedVertex) == 52);

	struct FStaticMeshLODResources
	{
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> TexCoords;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		std::vector<FStaticMeshSection> Sections;
		FBox LocalBounds;
		FBufferRHIRef PositionVertexBufferRHI;
		FBufferRHIRef StaticMeshVertexBufferRHI;
		FBufferRHIRef IndexBufferRHI;
		uint8 NumTexCoords = 0;
		bool bHasVertexColors = false;

		auto GetNumVertices() const -> uint32 { return static_cast<uint32>(Positions.size()); }
		auto GetNumIndices() const -> uint32 { return static_cast<uint32>(Indices.size()); }
	};

	class FRHICommandListImmediate;

	ENGINE_API auto PackStaticMeshVertex(
		const FVector3f& Normal,
		const FVector4f& Tangent,
		const std::array<FVector2f, MaxStaticMeshUVChannels>& TexCoords,
		const FVector4f& Color) -> FStaticMeshPackedVertex;

	struct FStaticMeshRenderData
	{
		std::vector<FStaticMeshLODResources> LODResources;
		std::vector<FStaticMeshMaterialSlot> MaterialSlots;
		FBox LocalBounds;

		ENGINE_API auto InitResources(FRHICommandListImmediate& RHICmdList) -> void;
		ENGINE_API auto ReleaseResources() -> void;
		ENGINE_API auto IsReadyForRendering(uint32 LODIndex = 0) const -> bool;
		ENGINE_API auto RecalculateBounds() -> void;
	};
}
