#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "SceneView.h"
#include "Materials/MaterialRenderProxy.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin
{
	class DMaterialInterface;

	// Owns persistent view policy and builds transient render snapshots for one viewport.
	class FViewportClient
	{
	public:
		ENGINE_API virtual ~FViewportClient();
		ENGINE_API virtual auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool;

		ENGINE_API auto SetPostProcessMaterial(DMaterialInterface* Material, FMaterialRenderProxyRef Proxy = {}) -> void;

		auto GetViewSettings() const -> const FSceneViewSettings& { return ViewSettings; }
		auto SetViewSettings(const FSceneViewSettings& InSettings) -> void { ViewSettings = InSettings; }

	private:
		FSceneViewSettings ViewSettings;
		TStrongObjectPtr<DMaterialInterface> PostProcessMaterial;
	};
}
