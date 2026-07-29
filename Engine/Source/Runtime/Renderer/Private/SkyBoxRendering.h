#pragma once

#include "SceneView.h"
#include "IScene.h"
#include "RendererAPI.h"

namespace Durin::SkyBoxRendering
{
	struct FSkyBoxUniform
	{
		glm::mat4 ClipToWorld{1.0f};
		glm::mat4 WorldToSky{1.0f};
		FVector4f ViewPosition{0.0f};
		FVector4f TintIntensity{1.0f};
	};

	// Builds the shader snapshot without retaining scene objects or camera translation
	// in the sky rotation transform.
	RENDERER_API auto BuildUniform(const FSceneView& View, const FSkyBoxSceneData& SkyBox, FSkyBoxUniform& OutUniform) -> bool;
}
