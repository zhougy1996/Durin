#pragma once

#include "Math/Operations.h"

namespace Durin::Editor::Material
{
	inline constexpr double MaterialPreviewFieldOfView = 42.0;

	// Fits a centered bounding sphere to both viewport axes with a 20% margin.
	inline auto CalculateMaterialPreviewDistance(double Radius, double AspectRatio) -> double
	{
		const double HalfAngle = std::atan(std::tan(Math::DegreesToRadians(MaterialPreviewFieldOfView) * 0.5)
			* std::min(1.0, AspectRatio) * 0.8);
		return Radius / std::sin(HalfAngle);
	}
}
