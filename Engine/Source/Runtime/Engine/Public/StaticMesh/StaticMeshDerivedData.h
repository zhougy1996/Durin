#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Serialization/SerializationDefinitions.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	class FArchive;
	inline constexpr uint32 StaticMeshPayloadSchemaVersion = 5;
	inline constexpr uint32 StaticMeshBuilderVersion = 4;
	inline constexpr uint32 StaticMeshPayloadAlignment = 16;
	inline constexpr uint32 StaticMeshPayloadHeaderSize = 64;
	inline constexpr uint32 StaticMeshPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumStaticMeshPayloadChunks = 64;
	inline constexpr uint32 MaximumStaticMeshLODs = 8;
	inline constexpr uint32 MaximumStaticMeshSectionsPerLOD = 65536;
	inline constexpr uint32 MaximumStaticMeshVerticesPerLOD = 100'000'000;
	inline constexpr uint32 MaximumStaticMeshIndicesPerLOD = 300'000'000;
	inline constexpr uint64 MaximumStaticMeshPayloadBytes = 8ull * 1024ull * 1024ull * 1024ull;
	inline const FGuid StaticMeshPrimaryCookedPayloadId{
		0x6d9f79b5, 0x7b684d91, 0xa42c2a60, 0x63fcab16};
	inline constexpr uint32 StaticMeshCollisionPayloadSchemaVersion = 2;
	inline constexpr uint32 StaticMeshCollisionBuilderVersion = 2;
	inline constexpr uint32 StaticMeshCollisionPayloadAlignment = 16;
	inline constexpr uint32 StaticMeshCollisionPayloadHeaderSize = 64;
	inline constexpr uint32 StaticMeshCollisionPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumStaticMeshCollisionPayloadChunks = 8;
	inline constexpr uint64 MaximumStaticMeshCollisionPayloadBytes = 256ull * 1024ull * 1024ull;
	inline const FGuid StaticMeshCollisionCookedPayloadId{
		0x3c10f7d1, 0x92fa4e20, 0xb544ad79, 0x1d788064};

	// Identifies a disk-compatible static-mesh build target independently of host platform enums.
	enum class EStaticMeshTargetPlatform : uint32
	{
		Unknown = 0,
		Win64 = 1
	};

	// Identifies one top-level record array in the version-one DMSH payload.
	enum class EStaticMeshPayloadChunkType : uint32
	{
		Bounds = 1,
		MaterialSlots = 2,
		LODs = 3,
		Sections = 4,
		VertexStreams = 5,
		IndexBuffers = 6
	};

	// Marks whether an unknown chunk may be skipped by a compatible reader.
	enum class EStaticMeshPayloadChunkFlags : uint32
	{
		None = 0,
		Required = 1
	};

	// Carries one disk-schema section without runtime-only names or resource handles.
	struct FStaticMeshPayloadSection
	{
		uint32 FirstIndex = 0;
		uint32 IndexCount = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		uint32 MaterialSlotIndex = 0;
		FBox LocalBounds;
	};

	// Carries the canonical CPU arrays encoded for one payload LOD.
	struct FStaticMeshPayloadLOD
	{
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> TexCoords;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		std::vector<FStaticMeshPayloadSection> Sections;
		FBox LocalBounds;
		float ScreenSize = 0.0f;
		uint8 NumTexCoords = 0;
		bool bHasVertexColors = false;
	};

	// Represents DMSH logical data independently of native C++ object layout and RHI state.
	struct FStaticMeshPayloadData
	{
		uint32 MaterialSlotCount = 0;
		std::vector<FStaticMeshPayloadLOD> LODs;
		FBox LocalBounds;

		ENGINE_API auto Serialize(
			FArchive& Ar,
			EStaticMeshTargetPlatform TargetPlatform) -> void;
	};

	struct FStaticMeshCollisionPayloadData
	{
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy QueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		std::vector<FVector3f> Positions;
		std::vector<uint32> Indices;
		std::vector<uint32> SourceOrdinals;
		std::vector<FCollisionGeometryNode> Nodes;
		std::vector<uint32> LeafTriangles;

		ENGINE_API auto Serialize(
			FArchive& Ar,
			EStaticMeshTargetPlatform TargetPlatform) -> void;
	};


	ENGINE_API auto MakeStaticMeshCollisionPayloadData(
		const FCollisionGeometryRef& Geometry,
		EBodySetupCollisionQueryPolicy QueryPolicy,
		FStaticMeshCollisionPayloadData& OutPayload,
		std::string& OutError) -> bool;
	ENGINE_API auto MakeStaticMeshCollisionGeometry(
		const FStaticMeshCollisionPayloadData& Payload,
		FCollisionGeometryRef& OutGeometry,
		std::string& OutError) -> bool;

	// Copies serializable CPU data from runtime render data into the explicit payload model.
	ENGINE_API auto MakeStaticMeshPayloadData(
		const FStaticMeshRenderData& RenderData,
		FStaticMeshPayloadData& OutPayload,
		std::string& OutError) -> bool;

	// Reconstructs CPU render data; runtime-only names and source material indices remain empty.
	ENGINE_API auto MakeStaticMeshRenderData(
		const FStaticMeshPayloadData& Payload,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::string& OutError) -> bool;
}
