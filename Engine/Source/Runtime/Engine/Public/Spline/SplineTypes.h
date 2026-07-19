#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "SplineTypes.gen.h"

namespace Durin
{
	DENUM()
	enum class ESplinePointType : uint8
	{
		Linear,
		Curve,
		CurveAuto,
		Constant
	};

	DSTRUCT()
	struct ENGINE_API FSplinePoint
	{
		GENERATED_BODY()

		FSplinePoint() = default;
		explicit FSplinePoint(const FVector3& InPosition)
			: Position(InPosition)
		{
		}

		DPROPERTY(Edit)
		FVector3 Position{0.0};

		// Tangents are derivatives with respect to a segment-local parameter in [0, 1].
		DPROPERTY(Edit)
		FVector3 ArriveTangent{100.0, 0.0, 0.0};

		DPROPERTY(Edit)
		FVector3 LeaveTangent{100.0, 0.0, 0.0};

		DPROPERTY(Edit)
		FQuat Rotation = glm::identity<FQuat>();

		DPROPERTY(Edit)
		FVector3 Scale{1.0};

		DPROPERTY(Edit)
		ESplinePointType Type = ESplinePointType::CurveAuto;

		auto operator==(const FSplinePoint&) const -> bool = default;
	};
} // namespace Durin
