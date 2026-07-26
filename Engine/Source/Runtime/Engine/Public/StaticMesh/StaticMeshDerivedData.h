#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	inline constexpr uint32 StaticMeshPayloadMagic = 0x48534D44; // DMSH
	inline constexpr uint32 StaticMeshPayloadSchemaVersion = 1;
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
}
