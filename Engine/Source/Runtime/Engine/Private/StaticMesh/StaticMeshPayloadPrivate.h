#pragma once

#include "CoreMinimal.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin::StaticMeshPrivate
{
	enum class EStaticMeshPayloadError : uint8
	{
		Bounds,
		MaterialSlotCount,
		LODCount,
		ScreenSize,
		ScreenSizeOrder,
		VertexCount,
		IndexCount,
		SectionCount,
		UVChannelCount,
		StoredSize,
		VertexStreamCount,
		UVStreamCount,
		ColorStreamCount,
		NonFiniteAttribute,
		NonFiniteUV,
		IndexRange,
		SectionCoverage,
		SectionVertexRange,
		SectionMaterialSlot,
		SectionBounds,
		SectionVertexMismatch,
		IncompleteCoverage,
		FinalScreenSize
	};

	enum class EStaticMeshPayloadStream : uint8
	{
		None,
		Position,
		Normal,
		Tangent,
		Color,
		UV,
		Index
	};

	// Owns rejected values; no references into a caller's mutable payload survive.
	struct FStaticMeshPayloadError
	{
		EStaticMeshPayloadError Code = EStaticMeshPayloadError::Bounds;
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
	ENGINE_API auto TryMakeStaticMeshPayloadData(const FStaticMeshRenderData& RenderData, FStaticMeshPayloadData& OutPayload) -> std::expected<void, FStaticMeshPayloadError>;
	ENGINE_API auto TryValidateStaticMeshRenderData(const FStaticMeshRenderData& RenderData)
		-> std::expected<void, FStaticMeshPayloadError>;
	ENGINE_API auto TryMakeStaticMeshRenderData(const FStaticMeshPayloadData& Payload, std::unique_ptr<FStaticMeshRenderData>& OutRenderData) -> std::expected<void, FStaticMeshPayloadError>;
} // namespace Durin::StaticMeshPrivate
