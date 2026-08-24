#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
#include <vector>

namespace Durin
{
	enum class EMeshBasePass : uint8
	{
		Opaque,
		Masked,
		Translucent,
	};

	enum class ERenderPreparationMode : uint8
	{
		Full,
		ShadowDepth,
	};

	enum class EVertexDeformationDomain : uint8
	{
		Local,
		Spline,
		Skeletal
	};

	struct FMeshShaderMapKey
	{
		FMaterialShaderMapIdentity Material;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		auto operator==(const FMeshShaderMapKey&) const -> bool = default;
	};

	struct FEffectiveMeshPipelineKey
	{
		FMaterialPlanningPassIdentity Material;
		FRHIRasterizerState Rasterizer;
		FRHIDepthStencilState Depth;
		FRHIColorBlendState ColorBlend;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		bool bHybridRetained = false;

		auto operator==(const FEffectiveMeshPipelineKey&) const -> bool = default;
	};

	// Complete value-only ordering facts. Stable identity is kept last so state
	// grouping happens before deterministic primitive/section tie breaking.
	struct FMeshDrawSortKey
	{
		std::array<uint32, 30> Pipeline{};
		std::vector<std::byte> MaterialUniform;
		std::array<uint32, 1 + MaxVertexElementCount * 5> VertexFactory{};
		std::array<uint32, 6> Geometry{};
		uint64 PrimitiveId = 0;
		uint32 SelectedLODIndex = 0;
		uint32 SectionIndex = 0;

		auto operator<=>(const FMeshDrawSortKey&) const = default;
	};

	struct FResolvedMeshDrawRecord
	{
		std::optional<FMaterialRenderBinding> MaterialBinding;
		bool bReady = false;
	};

	template <typename... TBuckets>
	auto AssignResolvedIndices(TBuckets&... Buckets) -> uint32
	{
		uint32 NextIndex = 0;
		auto AssignBucket = [&NextIndex](auto& Bucket) {
			for (auto& Item : Bucket) Item.ResolvedIndex = NextIndex++;
		};
		(AssignBucket(Buckets), ...);
		return NextIndex;
	}
} // namespace Durin
