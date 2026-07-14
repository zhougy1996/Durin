#pragma once

#include "IRendererModule.h"
#include "RenderCoreAPI.h"

namespace Durin::SceneViewProjection
{
	RENDERCORE_API auto ProjectWorldToViewport(const FSceneView& View, const FVector3& WorldPosition, FVector2f& OutPosition) -> bool;
	RENDERCORE_API auto BuildViewportRay(const FSceneView& View, const FVector2f& ViewportPosition, FVector3& OutOrigin, FVector3& OutDirection) -> bool;
}
