#pragma once

#include "EngineAPI.h"
#include "Math/Box.h"
#include "Spline/SplineCurve.h"

namespace Durin
{
	// Orthonormal right-handed frame whose cross product Forward x Side equals Up.
	struct FSplineMeshFrame
	{
		FVector3 Forward{1.0, 0.0, 0.0};
		FVector3 Side{0.0, 1.0, 0.0};
		FVector3 Up{0.0, 0.0, 1.0};
	};

	struct FSplineMeshSample
	{
		FVector3 Position{0.0};
		FVector3 Derivative{0.0};
		FVector2 Scale{1.0};
		FVector2 Offset{0.0};
		double RollRadians = 0.0;
		FSplineMeshFrame Frame;
	};

	struct FSplineMeshTangentBasis
	{
		FVector3 Normal{0.0, 0.0, 1.0};
		FVector3 Tangent{1.0, 0.0, 0.0};
		double Handedness = 1.0;
	};

	// Implements the CPU authority shared by bounds, picking, collision, and shader parity tests.
	class FSplineMeshDeformer final
	{
	public:
		// Rejects non-finite inputs and a degenerate canonical forward extent without modifying OutParams.
		ENGINE_API static auto Normalize(const FSplineMeshParams& Params,
			FSplineMeshParams& OutParams, std::string* OutError = nullptr) -> bool;
		ENGINE_API static auto Evaluate(const FSplineMeshParams& Params, double T) -> FSplineMeshSample;
		ENGINE_API static auto DeformPosition(const FSplineMeshParams& Params,
			const FVector3& SourcePosition) -> FVector3;
		ENGINE_API static auto DeformDirection(const FSplineMeshParams& Params,
			const FVector3& SourcePosition, const FVector3& SourceDirection) -> FVector3;
		ENGINE_API static auto DeformNormal(const FSplineMeshParams& Params,
			const FVector3& SourcePosition, const FVector3& SourceNormal) -> FVector3;
		ENGINE_API static auto DeformTangentBasis(const FSplineMeshParams& Params,
			const FVector3& SourcePosition, const FVector3& SourceNormal,
			const FVector3& SourceTangent, double SourceHandedness) -> FSplineMeshTangentBasis;
		// Returns the cubic control hull expanded by a source cross-section radius valid for every T.
		ENGINE_API static auto ComputeConservativeBounds(const FSplineMeshParams& Params,
			const FBox& CanonicalSourceBounds) -> FBox;
	};

	struct FSplinePathFrameSample
	{
		FSplineParameter Parameter;
		double LocalDistance = 0.0;
		FSplineMeshFrame Frame;
	};

	// Stores deterministic parallel-transport frames over an immutable spline evaluation snapshot.
	class FSplinePathFrameData final
	{
	public:
		ENGINE_API static auto Build(const FSplineEvaluationData& Evaluation,
			const FVector3& SeedUp = FVectorConstants::Up) -> FSplinePathFrameData;
		auto GetSamples() const -> const std::vector<FSplinePathFrameSample>& { return Samples; }

	private:
		std::vector<FSplinePathFrameSample> Samples;
	};
} // namespace Durin
