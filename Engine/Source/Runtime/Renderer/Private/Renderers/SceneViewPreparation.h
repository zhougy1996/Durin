#pragma once

#include "RendererAPI.h"

namespace Durin
{
	struct FSceneView;
	RENDERER_API auto FitSceneViewToOutput(const FSceneView& View,
		uint32 Width, uint32 Height) -> FSceneView;
} // namespace Durin
