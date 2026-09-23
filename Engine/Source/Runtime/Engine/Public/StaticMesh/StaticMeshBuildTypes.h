#pragma once

#include <expected>

#include "EngineAPI.h"
#include "Asset/AssetBuildCacheWarning.h"
#include "Asset/AssetBuildTaskContext.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "StaticMesh/StaticMeshData.h"

namespace Durin
{
	enum class EStaticMeshRenderBuildError : uint8
	{
		None, MissingGeometry, VertexLimit, TriangleList, NonFinitePosition, IndexRange,
		WorkingSet, DuplicateMaterial, RenderLimits, MissingMaterial, EmptyGeometry,
		Bounds, Cancelled
	};
	struct FStaticMeshRenderBuildError
	{
		EStaticMeshRenderBuildError Code = EStaticMeshRenderBuildError::None;
		std::string MeshName;
		std::string SectionName;
		uint64 Index = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 VertexCount = 0;
		uint64 IndexCount = 0;
		FVector3f Position = FVector3f(0);
		FBox Bounds;
	};

	ENGINE_API auto FormatStaticMeshRenderBuildError(const FStaticMeshRenderBuildError& Error) -> std::string;

	inline constexpr size_t MaximumStaticMeshBuildDiagnosticBytes = 4096;

	struct FStaticMeshBuilderDescriptor
	{
		std::string ProducerIdentity;
		uint32 RenderBuilderVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty()
				&& RenderBuilderVersion != 0;
		}
	};

	// Fixed slot metadata only; material object bindings remain with the operation owner.
	struct FStaticMeshBuildMaterialSlot
	{
		FName Name;
		std::string SourceName;
		uint32 SourceMaterialIndex = 0;
	};

	struct FStaticMeshRenderBuildRequest
	{
		FStaticMeshGeometryReadHandle Geometry;
		std::span<const FStaticMeshBuildMaterialSlot> MaterialSlots;
		float NormalizedSize = 1.5f;
	};

	// Owns complete CPU streams and metadata; Engine assembles runtime resources.
	struct FStaticMeshRenderBuildProduct
	{
		std::vector<FStaticMeshBuildLOD> LODs;
		FBox LocalBounds;
	};

}
