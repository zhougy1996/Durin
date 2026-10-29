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
		if (!Participation.IsRelevant(Pass == EMeshPipelinePass::ShadowDepth)) return false;
		const bool bTranslucent = Material.ShaderMap.BlendMode == EMaterialBlendMode::Translucent;
		const bool bGBuffer = !bTranslucent && bSupportsGBuffer
			&& Material.ShaderMap.ShadingModel == EMaterialShadingModel::Lit;
		switch (Pass)
		{
		case EMeshPipelinePass::ShadowDepth: return !bTranslucent;
		case EMeshPipelinePass::GBuffer: return bGBuffer;
		case EMeshPipelinePass::HybridRetainedForward: return !bTranslucent && !bGBuffer;
		case EMeshPipelinePass::SortedTranslucency: return bTranslucent;
		default: return true;
		}
	}
}
