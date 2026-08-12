#pragma once

#include "Components/SceneComponent.h"
#include "Misc/EnumClassFlags.h"
#include "Spline/SplineCurve.h"

#include "SplineComponent.gen.h"

namespace Durin
{
	enum class ESplineCoordinateSpace : uint8
	{
		Local,
		World
	};

	enum class ESplineChangeFlags : uint8
	{
		None = 0,
		Topology = 1 << 0,
		Geometry = 1 << 1,
		Build = 1 << 2
	};
	ENUM_CLASS_FLAGS(ESplineChangeFlags);

	DCLASS()
	class DSplineComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		using FSplineMutationListener = std::function<void(
			uint64 Revision, ESplineChangeFlags Flags,
			std::shared_ptr<const FSplineEvaluationData> Evaluation)>;
		ENGINE_API explicit DSplineComponent(const FObjectInitializer& ObjectInitializer);

		auto GetSplineCurve() const -> const FSplineCurve& { return SplineCurve; }
		auto GetSplinePoints() const -> const std::vector<FSplinePoint>& { return SplineCurve.GetPoints(); }
		auto GetSplinePoint(uint32 PointIndex) const -> const FSplinePoint* { return SplineCurve.GetPoint(PointIndex); }
		auto GetNumSplinePoints() const -> uint32 { return SplineCurve.GetNumPoints(); }
		auto GetNumSplineSegments() const -> uint32 { return SplineCurve.GetNumSegments(); }

		ENGINE_API auto SetSplinePoints(std::vector<FSplinePoint> InPoints) -> void;
		ENGINE_API auto AddSplinePoint(FSplinePoint Point) -> uint32;
		ENGINE_API auto InsertSplinePoint(uint32 PointIndex, FSplinePoint Point) -> bool;
		ENGINE_API auto DuplicateSplinePoint(uint32 PointIndex) -> std::optional<uint32>;
		ENGINE_API auto UpdateSplinePoint(uint32 PointIndex, FSplinePoint Point) -> bool;
		ENGINE_API auto RemoveSplinePoint(uint32 PointIndex) -> bool;
		ENGINE_API auto MoveSplinePoint(uint32 FromIndex, uint32 ToIndex) -> bool;
		ENGINE_API auto ClearSplinePoints() -> void;

		auto IsClosedLoop() const -> bool { return SplineCurve.IsClosedLoop(); }
		ENGINE_API auto SetClosedLoop(bool bClosedLoop) -> void;

		ENGINE_API auto GetEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>;
		ENGINE_API auto GetSampleAtParameter(FSplineParameter Parameter,
			ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FSplineSample;
		ENGINE_API auto GetSampleAtLocalDistance(double LocalDistance,
			ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FSplineSample;
		ENGINE_API auto GetLocalDistanceAtParameter(FSplineParameter Parameter) const -> double;
		ENGINE_API auto GetParameterAtLocalDistance(double LocalDistance) const -> FSplineParameter;
		ENGINE_API auto FindNearestParameter(const FVector3& Position,
			ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FSplineParameter;
		auto GetLocalSplineLength() const -> double { return GetEvaluationData()->GetLocalLength(); }
		auto GetLocalSplineBounds() const -> FBox { return GetEvaluationData()->GetLocalBounds(); }

		auto GetSplineRevision() const -> uint64 { return SplineRevision; }
		auto GetLastSplineChangeFlags() const -> ESplineChangeFlags { return LastSplineChangeFlags; }
		ENGINE_API auto AddSplineMutationListener(FSplineMutationListener Listener) -> uint64;
		ENGINE_API auto RemoveSplineMutationListener(uint64 ListenerId) -> bool;

		ENGINE_API auto UpdateSpline(ESplineChangeFlags ChangeFlags = ESplineChangeFlags::Build) -> void;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		auto TransformSampleToWorld(const FSplineSample& LocalSample) const -> FSplineSample;

		DPROPERTY(Edit)
		FSplineCurve SplineCurve;

		std::atomic<std::shared_ptr<const FSplineEvaluationData>> EvaluationData;
		uint64 SplineRevision = 0;
		ESplineChangeFlags LastSplineChangeFlags = ESplineChangeFlags::None;
		std::vector<std::pair<uint64, FSplineMutationListener>> MutationListeners;
		uint64 NextMutationListenerId = 1;
		bool bPublishingMutation = false;
	};
} // namespace Durin
