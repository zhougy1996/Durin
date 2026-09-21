#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
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
	struct FStaticMeshPayloadLOD : FStaticMeshVertexData
	{
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

		// Loads in place; discard failures. The byte owner checks completion before publication.
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const std::function<bool()>& ShouldCancel = {}) -> void;
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

		// Loads in place; discard failures. The byte owner checks completion before publication.
		ENGINE_API auto Serialize(
			FArchive& Ar,
			const std::function<bool()>& ShouldCancel = {}) -> void;
	};


	enum class EStaticMeshCollisionPayloadOperation : uint8 { Extract, Construct };
	enum class EStaticMeshCollisionPayloadError : uint8
	{
		None, InvalidGeometry, InvalidVertex, FloatStorage, InvalidTriangle, InvalidNode,
		InvalidMembership, InconsistentCounts, NonFinitePosition, UnexpectedBvh,
		DuplicateOrdinal, UnknownOrdinal, InvalidSourceMode, InvalidTopology, Cancelled,
	};
	struct FStaticMeshCollisionPayloadError
	{
		EStaticMeshCollisionPayloadError Code = EStaticMeshCollisionPayloadError::None;
		EStaticMeshCollisionPayloadOperation Operation = EStaticMeshCollisionPayloadOperation::Construct;
		EBodySetupCollisionSourceMode SourceMode = EBodySetupCollisionSourceMode::None;
		std::optional<ECollisionGeometryKind> GeometryKind;
		uint64 VertexCount = 0;
		uint64 IndexCount = 0;
		uint64 OrdinalCount = 0;
		uint64 NodeCount = 0;
		uint64 LeafCount = 0;
		uint64 Index = 0;
		uint32 Ordinal = 0;
		FVector3 Position = FVector3(0);
	};
	struct FStaticMeshCollisionPayloadResult
	{
		FStaticMeshCollisionPayloadError Error;
		explicit operator bool() const { return Error.Code == EStaticMeshCollisionPayloadError::None; }
	};
	ENGINE_API auto FormatStaticMeshCollisionPayloadError(const FStaticMeshCollisionPayloadError& Error) -> std::string;

	ENGINE_API auto MakeStaticMeshCollisionPayloadData(
		const FCollisionGeometryRef& Geometry,
		EBodySetupCollisionQueryPolicy QueryPolicy,
		FStaticMeshCollisionPayloadData& OutPayload,
		const std::function<bool()>& ShouldCancel = {}) -> FStaticMeshCollisionPayloadResult;
	ENGINE_API auto MakeStaticMeshCollisionGeometry(
		const FStaticMeshCollisionPayloadData& Payload,
		FCollisionGeometryRef& OutGeometry,
		const std::function<bool()>& ShouldCancel = {}) -> FStaticMeshCollisionPayloadResult;

	enum class EStaticMeshPayloadError : uint8
	{
		None, Bounds, MaterialSlotCount, LODCount, ScreenSize, ScreenSizeOrder,
		VertexCount, IndexCount, SectionCount, UVChannelCount, StoredSize,
		VertexStreamCount, UVStreamCount, ColorStreamCount, NonFiniteAttribute,
		NonFiniteUV, IndexRange, SectionCoverage, SectionVertexRange,
		SectionMaterialSlot, SectionBounds, SectionVertexMismatch,
		IncompleteCoverage, FinalScreenSize, Cancelled
	};

	enum class EStaticMeshPayloadStream : uint8 { None, Position, Normal, Tangent, Color, UV, Index };

	// Owns rejected values; no references into a caller's mutable payload survive.
	struct FStaticMeshPayloadError
	{
		EStaticMeshPayloadError Code = EStaticMeshPayloadError::None;
		EStaticMeshPayloadStream Stream = EStaticMeshPayloadStream::None;
		std::optional<uint64> LODIndex;
		std::optional<uint64> SectionIndex;
		std::optional<uint64> ElementIndex;
		std::optional<uint32> Channel;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 AdditionalActual = 0;
		uint64 AdditionalExpected = 0;
		float ScreenSize = 0.0f;
		float PreviousScreenSize = 0.0f;
		FBox Bounds;
		FVector4 Value = FVector4(0);
		std::optional<FStaticMeshPayloadSection> Section;
	};

	struct FStaticMeshPayloadResult
	{
		FStaticMeshPayloadError Error;
		explicit operator bool() const { return Error.Code == EStaticMeshPayloadError::None; }
	};

	ENGINE_API auto FormatStaticMeshPayloadError(const FStaticMeshPayloadError& Error) -> std::string;

	enum class EArchiveFailureCode : uint8;
	enum class EStaticMeshCacheCodecError : uint8 { None, RenderPayload, CollisionPayload, Archive, MaterialSlots, CollisionMetadata };
	enum class EStaticMeshCacheCodecOperation : uint8 { EncodeRender, DecodeRender, EncodeCollision, DecodeCollision };
	struct FStaticMeshCacheCodecError
	{
		EStaticMeshCacheCodecError Code = EStaticMeshCacheCodecError::None;
		EStaticMeshCacheCodecOperation Operation = EStaticMeshCacheCodecOperation::EncodeRender;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		EBodySetupCollisionSourceMode ActualMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionSourceMode ExpectedMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy ActualPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		EBodySetupCollisionQueryPolicy ExpectedPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		std::optional<FStaticMeshPayloadError> RenderCause;
		std::optional<FStaticMeshCollisionPayloadError> CollisionCause;
	};
	struct FStaticMeshCacheCodecResult
	{
		FStaticMeshCacheCodecError Error;
		explicit operator bool() const { return Error.Code == EStaticMeshCacheCodecError::None; }
	};
	ENGINE_API auto FormatStaticMeshCacheCodecError(const FStaticMeshCacheCodecError& Error) -> std::string;

	enum class EArchiveFailureCode : uint8;
	enum class EStaticMeshBuildKeyError : uint8 { None, UnsupportedTarget, Archive };
	struct FStaticMeshBuildKeyError
	{
		EStaticMeshBuildKeyError Code = EStaticMeshBuildKeyError::None;
		EStaticMeshTargetPlatform TargetPlatform = EStaticMeshTargetPlatform::Unknown;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
	};

	// Copies serializable CPU data from runtime render data into the explicit payload model.
	ENGINE_API auto MakeStaticMeshPayloadData(
		const FStaticMeshRenderData& RenderData,
		FStaticMeshPayloadData& OutPayload,
		const std::function<bool()>& ShouldCancel = {}) -> FStaticMeshPayloadResult;

	// Reconstructs CPU render data; runtime-only names and source material indices remain empty.
	ENGINE_API auto MakeStaticMeshRenderData(
		const FStaticMeshPayloadData& Payload,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		const std::function<bool()>& ShouldCancel = {}) -> FStaticMeshPayloadResult;
}
