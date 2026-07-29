#pragma once

#include "EngineAPI.h"
#include "SceneView.h"

namespace Durin
{
	// Owns persistent view policy and builds transient render snapshots for one viewport.
	class FViewportClient
	{
	public:
		ENGINE_API virtual ~FViewportClient();
		ENGINE_API virtual auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool;

		auto GetViewSettings() const -> const FSceneViewSettings& { return ViewSettings; }
		auto SetViewSettings(const FSceneViewSettings& InSettings) -> void { ViewSettings = InSettings; }

	private:
		FSceneViewSettings ViewSettings;
	};
}
