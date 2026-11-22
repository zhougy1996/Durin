#pragma once

#include "CoreMinimal.h"
#include "Renderers/BaseSceneRendering.h"
#include "Renderers/VolumetricCloudRendering.h"

namespace Durin
{
	class FPostProcessRenderer;
	struct FResolvedSceneResources;
	struct FSceneEnvironmentInputs;
	struct FRendererQualificationPolicy;

	auto AddSingleLayerWaterPasses(FRDGBuilder& Graph, const FSceneView& View,
		const FBaseSceneGraphOutput& Base, const FCloudCompositeGraphOutput& Cloud,
		const FResolvedSceneResources& Resolved, FPostProcessRenderer& Fullscreen,
		const FSceneEnvironmentInputs& Environment, std::optional<FRDGTextureHandle> DirectionalShadow,
		uint32 Width, uint32 Height, const FRendererQualificationPolicy& Qualification) -> std::optional<TRDGValueHandle<FSceneColorPassResult>>;
}
