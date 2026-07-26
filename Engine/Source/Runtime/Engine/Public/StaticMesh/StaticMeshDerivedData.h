#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	inline constexpr uint32 StaticMeshPayloadMagic = 0x48534D44; // DMSH
	inline constexpr uint32 StaticMeshPayloadSchemaVersion = 2;
	inline constexpr uint32 StaticMeshBuilderVersion = 1;
	inline constexpr uint32 StaticMeshDerivedDataKeySchemaVersion = 1;
	inline constexpr uint32 StaticMeshPayloadAlignment = 16;
	inline constexpr uint32 StaticMeshPayloadHeaderSize = 64;
	inline constexpr uint32 StaticMeshPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumStaticMeshPayloadChunks = 64;
	inline constexpr uint32 MaximumStaticMeshLODs = 8;
	inline constexpr uint32 MaximumStaticMeshMaterialSlots = 4096;
	inline constexpr uint32 MaximumStaticMeshSectionsPerLOD = 65536;
	inline constexpr uint32 MaximumStaticMeshVerticesPerLOD = 100'000'000;
	inline constexpr uint32 MaximumStaticMeshIndicesPerLOD = 300'000'000;
	inline constexpr uint64 MaximumStaticMeshPayloadBytes = 8ull * 1024ull * 1024ull * 1024ull;
	inline const FGuid StaticMeshPrimaryCookedPayloadId{
		0x6d9f79b5, 0x7b684d91, 0xa42c2a60, 0x63fcab16};

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
		uint8 NumTexCoords = 0;
		bool bHasVertexColors = false;
	};

	// Represents DMSH logical data independently of native C++ object layout and RHI state.
	struct FStaticMeshPayloadData
	{
		std::vector<FGuid> MaterialSlotIds;
		std::vector<FStaticMeshPayloadLOD> LODs;
		FBox LocalBounds;
	};

	// Collects every semantic input to the version-one static-mesh derived-data key.
	struct FStaticMeshDerivedDataKeyInput
	{
		FXxHash128 SourceContentHash;
		std::string ImporterId;
		uint32 ImporterVersion = 0;
		FStaticMeshImportSettings ImportSettings;
		uint32 BuilderVersion = StaticMeshBuilderVersion;
		uint32 PayloadSchemaVersion = StaticMeshPayloadSchemaVersion;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;
	};

	// Produces the canonical bytes hashed for a static-mesh DDC key.
	ENGINE_API auto BuildStaticMeshDerivedDataKeyBytes(
		const FStaticMeshDerivedDataKeyInput& Input) -> std::vector<uint8>;

	// Returns the lowercase 32-hex-character XXH3-128 key for the canonical input bytes.
	ENGINE_API auto BuildStaticMeshDerivedDataKey(
		const FStaticMeshDerivedDataKeyInput& Input) -> std::string;

	// Encodes a validated logical mesh into deterministic DMSH schema-version-two bytes.
	ENGINE_API auto EncodeStaticMeshPayload(
		const FStaticMeshPayloadData& Payload,
		EStaticMeshTargetPlatform TargetPlatform,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;

	// Decodes DMSH bytes transactionally; OutPayload is unchanged when validation fails.
	ENGINE_API auto DecodeStaticMeshPayload(
		std::span<const uint8> Bytes,
		EStaticMeshTargetPlatform ExpectedPlatform,
		FStaticMeshPayloadData& OutPayload,
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
