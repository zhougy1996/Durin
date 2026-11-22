#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialRenderTypes.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Rendering/PrimitiveRenderParticipation.h"

namespace Durin::RendererPrivate
{
	// Shared material eligibility for drawing and automatic PSO collection.
	inline auto IsMeshPassRelevant(EMeshPipelinePass Pass, const FMaterialPlanningPassIdentity& Material,
		const FPrimitiveRenderParticipation& Participation = {}, bool bSupportsGBuffer = true) -> bool
	{
		if (!Participation.IsRelevant(Pass == EMeshPipelinePass::ShadowDepth
			? EMeshCollectionPurpose::Shadow : EMeshCollectionPurpose::Receiver)) return false;
		const bool bTranslucent = Material.ShaderMap.BlendMode == EMaterialBlendMode::Translucent;
		const bool bWater = Material.ShaderMap.ShadingModel == EMaterialShadingModel::SingleLayerWater;
		const bool bGBuffer = !bTranslucent && bSupportsGBuffer
							  && (Material.ShaderMap.ShadingModel == EMaterialShadingModel::DefaultLit
								  || Material.ShaderMap.ShadingModel == EMaterialShadingModel::ClearCoat);
		switch (Pass)
		{
		case EMeshPipelinePass::ShadowDepth: return !bTranslucent;
		case EMeshPipelinePass::GBuffer: return bGBuffer;
		case EMeshPipelinePass::HybridRetainedForward: return !bTranslucent && !bGBuffer && !bWater;
		case EMeshPipelinePass::SortedTranslucency: return bTranslucent;
		case EMeshPipelinePass::SingleLayerWater:
		case EMeshPipelinePass::SingleLayerWaterDepth: return bWater && !bTranslucent;
		default: return true;
		}
	}
}
