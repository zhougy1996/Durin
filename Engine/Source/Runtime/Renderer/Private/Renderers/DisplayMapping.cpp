#include "Renderers/DisplayMapping.h"

#include <algorithm>
#include <cmath>

namespace Durin::DisplayMapping
{
	auto CanonicalizeExposureEV(float ExposureEV) -> float
	{
		if (!std::isfinite(ExposureEV)) return DefaultExposureEV;
		return std::clamp(ExposureEV, MinimumExposureEV, MaximumExposureEV);
	}

	auto CalculateExposureScale(float ExposureEV) -> float
	{
		return std::exp2(CanonicalizeExposureEV(ExposureEV));
	}

	auto MapSceneLinearToDisplayLinear(
		const FVector3f& SceneLinear,
		float ExposureEV) -> FVector3f
	{
		const float ExposureScale = CalculateExposureScale(ExposureEV);
		auto Map = [ExposureScale](float Channel) {
			const float Exposed = Channel * ExposureScale;
			const float NonNegative = std::max(Exposed, 0.0f);
			const float Numerator =
				NonNegative * (ACESA * NonNegative + ACESB);
			const float Denominator =
				NonNegative * (ACESC * NonNegative + ACESD) + ACESE;
			const float Mapped = Numerator / Denominator;
			return std::isfinite(Mapped)
				? std::clamp(Mapped, 0.0f, 1.0f)
				: 0.0f;
		};
		return {Map(SceneLinear.x), Map(SceneLinear.y), Map(SceneLinear.z)};
	}

	auto MapOutputAlpha(float SceneAlpha) -> float
	{
		return std::isfinite(SceneAlpha)
			? std::clamp(SceneAlpha, 0.0f, 1.0f)
			: 0.0f;
	}
} // namespace Durin::DisplayMapping
