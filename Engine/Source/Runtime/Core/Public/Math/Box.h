#pragma once

#include "Math/Vector.h"

namespace Durin
{
	struct FBox
	{
		FVector3 Min{0.0};
		FVector3 Max{0.0};
		bool bIsValid = false;

		FBox() = default;
		FBox(const FVector3& InMin, const FVector3& InMax)
			: Min(InMin), Max(InMax), bIsValid(glm::all(glm::lessThanEqual(InMin, InMax)))
		{
		}

		auto Reset() -> void
		{
			Min = FVector3(0.0);
			Max = FVector3(0.0);
			bIsValid = false;
		}

		auto AddPoint(const FVector3& Point) -> void
		{
			if (!std::isfinite(Point.x) || !std::isfinite(Point.y) || !std::isfinite(Point.z)) return;
			if (!bIsValid)
			{
				Min = Point;
				Max = Point;
				bIsValid = true;
				return;
			}
			Min = glm::min(Min, Point);
			Max = glm::max(Max, Point);
		}

		auto GetCenter() const -> FVector3 { return (Min + Max) * 0.5; }
		auto GetExtent() const -> FVector3 { return (Max - Min) * 0.5; }
	};
}
