#pragma once

#include <expected>

#include "EngineAPI.h"
#include "Asset/PayloadTargetPlatform.h"
#include "Hash/XxHash.h"
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

	ENGINE_API auto FormatStaticMeshPayloadError(const FStaticMeshPayloadError& Error) -> std::string;

	enum class EArchiveFailureCode : uint8;
	enum class EStaticMeshCacheCodecError : uint8 { None, RenderPayload, Archive, MaterialSlots };
	enum class EStaticMeshCacheCodecOperation : uint8 { EncodeRender, DecodeRender };
	struct FStaticMeshCacheCodecError
	{
		EStaticMeshCacheCodecError Code = EStaticMeshCacheCodecError::None;
		EStaticMeshCacheCodecOperation Operation = EStaticMeshCacheCodecOperation::EncodeRender;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		std::optional<FStaticMeshPayloadError> RenderCause;
	};

	ENGINE_API auto FormatStaticMeshCacheCodecError(const FStaticMeshCacheCodecError& Error) -> std::string;

	enum class EArchiveFailureCode : uint8;
	enum class EStaticMeshBuildKeyError : uint8 { None, UnsupportedTarget, Archive };
	struct FStaticMeshBuildKeyError
	{
		EStaticMeshBuildKeyError Code = EStaticMeshBuildKeyError::None;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Unknown;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
	};

	// Copies serializable CPU data from runtime render data into the explicit payload model.
	ENGINE_API auto MakeStaticMeshPayloadData(
		const FStaticMeshRenderData& RenderData,
		FStaticMeshPayloadData& OutPayload,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<void, FStaticMeshPayloadError>;

	// Reconstructs CPU render data; runtime-only names and source material indices remain empty.
	ENGINE_API auto MakeStaticMeshRenderData(
		const FStaticMeshPayloadData& Payload,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<void, FStaticMeshPayloadError>;
}
