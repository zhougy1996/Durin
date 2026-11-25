#pragma once

#include "CoreMinimal.h"

#include "RendererAPI.h"

#include "Rendering/SkyBoxSceneProxy.h"
#include "SceneView.h"

namespace Durin::SkyBoxRendering
{
	struct FSkyBoxUniform
	{
		// Homogeneous clip X, Y and W rows. The last row also carries far depth.
		std::array<FVector4f, 3> DomeProjection{};
		FVector4f TintIntensity{1.0f};
		FProceduralSkyUniform ProceduralSky;
	};

	// Builds the shader snapshot without retaining scene objects or camera
	// translation in the GPU direction reconstruction.
	RENDERER_API auto BuildUniform(const FSceneView& View, const FSkyBoxSceneData& SkyBox, FSkyBoxUniform& OutUniform) -> bool;
}
