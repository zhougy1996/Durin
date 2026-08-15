#include "Components/SplineComponent.h"

#include "DObject/Property.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		auto SafeNormalize(const FVector3& Value) -> FVector3
		{
			const double LengthSquared = Math::LengthSquared(Value);
			return LengthSquared > kSmallNumber ? Value / std::sqrt(LengthSquared) : FVectorConstants::Zero;
		}
	} // namespace

	DSplineComponent::DSplineComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
	}

	auto DSplineComponent::SetSplinePoints(std::vector<FSplinePoint> InPoints) -> void
	{
		if (SplineCurve.GetPoints() == InPoints) return;
		SplineCurve.SetPoints(std::move(InPoints));
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
	}

	auto DSplineComponent::AddSplinePoint(FSplinePoint Point) -> uint32
	{
		const uint32 PointIndex = SplineCurve.AddPoint(std::move(Point));
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
		return PointIndex;
	}

	auto DSplineComponent::InsertSplinePoint(uint32 PointIndex, FSplinePoint Point) -> bool
	{
		if (!SplineCurve.InsertPoint(PointIndex, std::move(Point))) return false;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::DuplicateSplinePoint(uint32 PointIndex) -> std::optional<uint32>
	{
		const std::optional<uint32> DuplicateIndex = SplineCurve.DuplicatePoint(PointIndex);
		if (!DuplicateIndex) return std::nullopt;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
		return DuplicateIndex;
	}

	auto DSplineComponent::UpdateSplinePoint(uint32 PointIndex, FSplinePoint Point) -> bool
	{
		if (!SplineCurve.UpdatePoint(PointIndex, std::move(Point))) return false;
		UpdateSpline(ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::RemoveSplinePoint(uint32 PointIndex) -> bool
	{
		if (!SplineCurve.RemovePoint(PointIndex)) return false;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::MoveSplinePoint(uint32 FromIndex, uint32 ToIndex) -> bool
	{
		if (!SplineCurve.MovePoint(FromIndex, ToIndex)) return false;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::ClearSplinePoints() -> void
	{
		if (SplineCurve.GetNumPoints() == 0) return;
		SplineCurve.ClearPoints();
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
	}

	auto DSplineComponent::SetClosedLoop(bool bClosedLoop) -> void
	{
		if (SplineCurve.IsClosedLoop() == bClosedLoop) return;
		SplineCurve.SetClosedLoop(bClosedLoop);
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
	}

	auto DSplineComponent::GetEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>
	{
		return std::atomic_load_explicit(&EvaluationData, std::memory_order_acquire);
	}

	auto DSplineComponent::GetSampleAtParameter(FSplineParameter Parameter, ESplineCoordinateSpace Space) const -> FSplineSample
	{
		const FSplineSample LocalSample = GetEvaluationData()->Evaluate(Parameter);
		return Space == ESplineCoordinateSpace::Local ? LocalSample : TransformSampleToWorld(LocalSample);
	}

	auto DSplineComponent::GetSampleAtLocalDistance(double LocalDistance, ESplineCoordinateSpace Space) const -> FSplineSample
	{
		const FSplineSample LocalSample = GetEvaluationData()->EvaluateAtLocalDistance(LocalDistance);
		return Space == ESplineCoordinateSpace::Local ? LocalSample : TransformSampleToWorld(LocalSample);
	}

	auto DSplineComponent::GetLocalDistanceAtParameter(FSplineParameter Parameter) const -> double
	{
		return GetEvaluationData()->GetLocalDistanceAtParameter(Parameter);
	}

	auto DSplineComponent::GetParameterAtLocalDistance(double LocalDistance) const -> FSplineParameter
	{
		return GetEvaluationData()->GetParameterAtLocalDistance(LocalDistance);
	}

	auto DSplineComponent::FindNearestParameter(const FVector3& Position, ESplineCoordinateSpace Space) const -> FSplineParameter
	{
		FVector3 LocalPosition = Position;
		if (Space == ESplineCoordinateSpace::World)
		{
			LocalPosition = FVector3(Math::Inverse(GetComponentToWorldMatrix()) * FVector4(Position, 1.0));
		}
		return GetEvaluationData()->FindNearestParameter(LocalPosition);
	}

	auto DSplineComponent::UpdateSpline(ESplineChangeFlags ChangeFlags) -> void
	{
		checkf(!bPublishingMutation, "Spline mutation listeners must not mutate the publishing Spline component reentrantly.");
		const bool bRepairedIds = SplineCurve.RepairPointIds();
		if (bRepairedIds) ChangeFlags |= ESplineChangeFlags::Topology;
		const auto PublishedEvaluation = SplineCurve.BuildEvaluationData();
		std::atomic_store_explicit(&EvaluationData, PublishedEvaluation, std::memory_order_release);
		++SplineRevision;
		LastSplineChangeFlags = ChangeFlags;
		bPublishingMutation = true;
		std::vector<uint64> ListenerIds;
		ListenerIds.reserve(MutationListeners.size());
		for (const auto& Entry : MutationListeners) ListenerIds.push_back(Entry.first);
		for (uint64 ListenerId : ListenerIds)
		{
			const auto Found = std::ranges::find(MutationListeners, ListenerId,
				[](const auto& Entry) { return Entry.first; });
			if (Found != MutationListeners.end() && Found->second)
				Found->second(SplineRevision, ChangeFlags, PublishedEvaluation);
		}
		bPublishingMutation = false;
	}

	auto DSplineComponent::AddSplineMutationListener(FSplineMutationListener Listener) -> uint64
	{
		if (!Listener) return 0;
		const uint64 Id = NextMutationListenerId++;
		MutationListeners.emplace_back(Id, std::move(Listener));
		return Id;
	}

	auto DSplineComponent::RemoveSplineMutationListener(uint64 ListenerId) -> bool
	{
		return std::erase_if(MutationListeners,
			[ListenerId](const auto& Entry) { return Entry.first == ListenerId; }) != 0;
	}

	auto DSplineComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		return true;
	}

	auto DSplineComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		return Super::PreEditChangeProperty(Proposal, OutError);
	}

	auto DSplineComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("SplineCurve"))
		{
			const ESplineChangeFlags Flags = Event.Kind == EPropertyChangeKind::ValueSet
				? ESplineChangeFlags::Geometry | ESplineChangeFlags::Build
				: ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build;
			UpdateSpline(Flags);
		}
	}

	auto DSplineComponent::TransformSampleToWorld(const FSplineSample& LocalSample) const -> FSplineSample
	{
		FSplineSample Result;
		Result.Position = FVector3(GetComponentToWorldMatrix() * FVector4(LocalSample.Position, 1.0));
		Result.FirstDerivative = GetWorldRotation() * (GetWorldScale3D() * LocalSample.FirstDerivative);
		Result.SecondDerivative = GetWorldRotation() * (GetWorldScale3D() * LocalSample.SecondDerivative);
		Result.Direction = SafeNormalize(Result.FirstDerivative);
		return Result;
	}
} // namespace Durin
