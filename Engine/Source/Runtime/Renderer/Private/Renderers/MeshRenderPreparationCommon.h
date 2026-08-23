#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"

#include <array>
#include <compare>
#include <cstddef>
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
		FMaterialPipelineIdentity Material;
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
		std::array<uint32, 26> Pipeline{};
		std::vector<std::byte> MaterialUniform;
		std::array<uint32, 1 + MaxVertexElementCount * 5> VertexFactory{};
		std::array<uint32, 6> Geometry{};
		uint64 PrimitiveId = 0;
		uint32 SelectedLODIndex = 0;
		uint32 SectionIndex = 0;

		auto operator<=>(const FMeshDrawSortKey&) const = default;
	};
} // namespace Durin
