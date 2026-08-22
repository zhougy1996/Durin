#pragma once

#include "IScene.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/VolumetricCloudSpatialRenderer.h"

namespace Durin
{
	// Translates Engine-owned authoring intent into the frozen P1 value contract.
	RENDERER_API auto BuildVolumetricCloudParameters(
		const FVolumetricCloudSceneData& Cloud,
		const FPreparedLightView& Lights)
		-> FVolumetricCloudSpatialRenderer::FParameters;
}
