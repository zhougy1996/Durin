#pragma once

#include "RendererAPI.h"

#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"
#include "Scene.h"
#include "SceneView.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	enum class EStaticMeshBasePass : uint8
	{
		Opaque,
		Masked,
		Translucent,
	};

	struct FEffectiveStaticMeshPipelineKey
	{
		FMaterialPipelineIdentity Material;
		FRHIRasterizerState Rasterizer;
		FRHIDepthState Depth;
		FRHIColorBlendState ColorBlend;

		auto operator==(const FEffectiveStaticMeshPipelineKey&) const
			-> bool = default;
	};

	struct FPreparedStaticMeshSection
	{
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		uint32 SectionIndex = 0;
		const FPrimitiveSceneInfo* SceneInfo = nullptr;
		const FStaticMeshSceneProxy* Proxy = nullptr;
		const FStaticMeshRenderData* RenderData = nullptr;
		const FStaticMeshLODResources* LOD = nullptr;
		const FStaticMeshSection* Section = nullptr;
		FMatrix LocalToWorld{1.0};
		FVector3 SortCenter{0.0};
		double TranslucentDistanceSquared = 0.0;
		FMaterialRenderData Material;
		FMaterialRenderV3Binding MaterialBinding;
		EStaticMeshBasePass Pass = EStaticMeshBasePass::Opaque;
		FMaterialShaderMapIdentity ShaderMapIdentity;
		FEffectiveStaticMeshPipelineKey PipelineKey;
	};

	struct FPreparedStaticMeshView
	{
		std::vector<FPreparedStaticMeshSection> Opaque;
		std::vector<FPreparedStaticMeshSection> Masked;
		std::vector<FPreparedStaticMeshSection> Translucent;

		auto GetNumSections() const -> size_t
		{
			return Opaque.size() + Masked.size() + Translucent.size();
		}
	};

	RENDERER_API auto PrepareStaticMeshView_RenderThread(
		const FScene& Scene,
		const FSceneView& View,
		ERasterMode RasterMode
	) -> FPreparedStaticMeshView;
} // namespace Durin
