#pragma once

#include "CoreMinimal.h"
#include "RHIFeatureLevel.h"

namespace Durin
{
	// Renderer policy: supported routes, independent of the route selected by a view.
	struct FRendererPreparationSettings
	{
		bool bForward = true;
		bool bHybrid = true;
		bool bHitProxy = true;
		auto operator==(const FRendererPreparationSettings&) const -> bool = default;
	};

	// Value snapshot shared by scene preparation and material PSO collection.
	// Version zero denotes an unconfigured host; native Renderer admission stamps it.
	struct FSceneRenderPreparationConfig
	{
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		FRendererPreparationSettings Settings;
		bool bSupportsWireframe = true;
		uint64 Version = 0;
		auto operator==(const FSceneRenderPreparationConfig&) const -> bool = default;
	};
}
