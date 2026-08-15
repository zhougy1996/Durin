#pragma once

#include "RendererAPI.h"
#include "Math/DurinMath.h"

namespace Durin::DisplayMapping
{
	inline constexpr float MinimumExposureEV = -16.0f;
	inline constexpr float MaximumExposureEV = 16.0f;
	inline constexpr float DefaultExposureEV = 0.0f;

	inline constexpr float ACESA = 2.51f;
	inline constexpr float ACESB = 0.03f;
	inline constexpr float ACESC = 2.43f;
	inline constexpr float ACESD = 0.59f;
	inline constexpr float ACESE = 0.14f;

	RENDERER_API auto CanonicalizeExposureEV(float ExposureEV) -> float;
	RENDERER_API auto CalculateExposureScale(float ExposureEV) -> float;
	RENDERER_API auto MapSceneLinearToDisplayLinear(
		const FVector3f& SceneLinear,
		float ExposureEV) -> FVector3f;
	RENDERER_API auto MapOutputAlpha(float SceneAlpha) -> float;
} // namespace Durin::DisplayMapping
