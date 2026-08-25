#pragma once

#include "EngineAPI.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
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

	// Selects the source-mesh axis mapped onto a Hermite segment direction.
	DENUM()
	enum class ESplineMeshAxis : uint8
	{
		X,
		Y,
		Z
	};

	// Controls interpolation of scale, roll, and offset independently of curve geometry.
	DENUM()
	enum class ESplineMeshInterpolation : uint8
	{
		Linear,
		SmoothStep
	};

	// Contains one finite, component-local SplineMesh deformation interval.
	DSTRUCT()
	struct FSplineMeshParams
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FVector3 StartPosition{0.0};

		DPROPERTY(Edit)
		FVector3 StartTangent{5.0, 0.0, 0.0};

		DPROPERTY(Edit)
		FVector3 EndPosition{5.0, 0.0, 0.0};

		DPROPERTY(Edit)
		FVector3 EndTangent{5.0, 0.0, 0.0};

		DPROPERTY(Edit)
		FVector2 StartScale{1.0};

		DPROPERTY(Edit)
		FVector2 EndScale{1.0};

		DPROPERTY(Edit)
		double StartRollRadians = 0.0;

		DPROPERTY(Edit)
		double EndRollRadians = 0.0;

		DPROPERTY(Edit)
		FVector2 StartOffset{0.0};

		DPROPERTY(Edit)
		FVector2 EndOffset{0.0};

		DPROPERTY(Edit)
		FVector3 SplineUpDirection{0.0, 0.0, 1.0};

		DPROPERTY(Edit)
		ESplineMeshAxis ForwardAxis = ESplineMeshAxis::X;

		DPROPERTY(Edit)
		ESplineMeshInterpolation Interpolation = ESplineMeshInterpolation::Linear;

		// Canonical LOD 0 source bounds along ForwardAxis; the interval must be non-degenerate.
		DPROPERTY()
		double SourceForwardMin = 0.0;

		DPROPERTY()
		double SourceForwardMax = 100.0;

		auto operator==(const FSplineMeshParams&) const -> bool = default;
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
		FVector3 ArriveTangent{5.0, 0.0, 0.0};

		DPROPERTY(Edit)
		FVector3 LeaveTangent{5.0, 0.0, 0.0};

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
