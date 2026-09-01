#pragma once

#include "Rendering/VolumetricCloudSceneProxy.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/VolumetricCloudSpatialRenderer.h"

namespace Durin
{
	// Translates Engine-owned authored intent into the frozen P1 value contract.
	RENDERER_API auto BuildVolumetricCloudParameters(
		const FVolumetricCloudSceneData& Cloud,
		const FPreparedLightView& Lights
	)
		-> FVolumetricCloudSpatialRenderer::FParameters;
	RENDERER_API auto CalculateVolumetricCloudLightingKey(
		const FPreparedLightView& Lights
	) -> uint64;
} // namespace Durin
