#pragma once

#include "EngineAPI.h"
namespace Durin
{
	struct FSceneView;

	// Builds a scene view for a viewport without owning the viewport or rendered scene.
	class FViewportClient
	{
	public:
		ENGINE_API virtual ~FViewportClient();
		ENGINE_API virtual auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool;
	};
}
