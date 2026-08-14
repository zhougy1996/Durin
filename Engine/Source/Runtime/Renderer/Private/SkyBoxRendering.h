#pragma once

#include "SceneView.h"
#include "IScene.h"
#include "RendererAPI.h"

namespace Durin::SkyBoxRendering
{
	struct FSkyBoxUniform
	{
		// Maps clip positions directly to translation-free sky directions. Building
		// this in double precision avoids subtracting two large, nearby world
		// positions in the fragment shader when the near clip is very small.
		FMatrix4f ClipToSkyDirection{1.0f};
		FVector4f TintIntensity{1.0f};
	};

	// Builds the shader snapshot without retaining scene objects or camera
	// translation in the GPU direction reconstruction.
	RENDERER_API auto BuildUniform(const FSceneView& View, const FSkyBoxSceneData& SkyBox, FSkyBoxUniform& OutUniform) -> bool;
}
