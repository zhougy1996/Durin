#pragma once

#include "Renderers/SceneVisibility.h"
#include "Renderers/StaticMeshRenderPreparation.h"

#include "EnvironmentLighting/EnvironmentLighting.h"
#include "IScene.h"
#include "SceneView.h"

#include <cstddef>
#include <vector>

namespace Durin
{
	class FTextureCubePreviewSceneProxy;

	// Command-local immutable scene data used by Scene Color execution.
	struct FPreparedSceneView
	{
		FSceneView View;
		FDirectionalLightSceneData DirectionalLight;
		FSkyBoxSceneData SkyBox;
		bool bHasSkyBox = false;
		FPreparedStaticMeshView StaticMeshes;
		std::vector<const FTextureCubePreviewSceneProxy*> TextureCubePreviews;
		FViewRenderCounters Counters;
	};
} // namespace Durin
