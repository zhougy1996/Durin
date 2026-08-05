#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Misc/Guid.h"

#include "SplineTypes.gen.h"

namespace Durin
{
	DENUM()
	enum class ESplineSegmentInterpolation : uint8
	{
		Linear,
		Cubic
	};

	DENUM()
	enum class ESplineTangentMode : uint8
	{
		Automatic,
		AutomaticClamped,
		ManualAligned,
		ManualBroken
	};

	DSTRUCT()
	struct FSplinePoint
	{
		GENERATED_BODY()

		ENGINE_API FSplinePoint();
		ENGINE_API explicit FSplinePoint(const FVector3& InPosition);

		DPROPERTY()
		FGuid Id;

		DPROPERTY(Edit)
		FVector3 Position{0.0};

		DPROPERTY(Edit)
		FVector3 ArriveTangent{100.0, 0.0, 0.0};

		DPROPERTY(Edit)
		FVector3 LeaveTangent{100.0, 0.0, 0.0};

		DPROPERTY(Edit)
		ESplineSegmentInterpolation OutgoingInterpolation = ESplineSegmentInterpolation::Cubic;

		DPROPERTY(Edit)
		ESplineTangentMode TangentMode = ESplineTangentMode::Automatic;

		auto operator==(const FSplinePoint&) const -> bool = default;
	};

	struct FSplineParameter
	{
		uint32 SegmentIndex = 0;
		double T = 0.0;

		auto operator==(const FSplineParameter&) const -> bool = default;
	};

	struct FSplineSample
	{
		FVector3 Position{0.0};
		FVector3 FirstDerivative{0.0};
		FVector3 SecondDerivative{0.0};
		FVector3 Direction{0.0};
	};
} // namespace Durin
