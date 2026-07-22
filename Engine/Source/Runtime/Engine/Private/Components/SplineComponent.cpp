#include "Components/SplineComponent.h"

#include "DObject/Property.h"

namespace Durin
{
	namespace
	{
		auto SafeNormalize(const FVector3& Value) -> FVector3
		{
			const double LengthSquared = glm::dot(Value, Value);
			return LengthSquared > kSmallNumber ? Value / std::sqrt(LengthSquared) : FVectorConstants::Zero;
		}
	}

	auto DSplineComponent::SetSplinePoints(std::vector<FSplinePoint> InPoints) -> void
	{
		SplineCurve.SetPoints(std::move(InPoints));
		UpdateSpline();
		MarkPackageDirty();
	}

	auto DSplineComponent::AddSplinePoint(const FSplinePoint& Point) -> uint32
	{
		const uint32 PointIndex = SplineCurve.AddPoint(Point);
		UpdateSpline();
		MarkPackageDirty();
		return PointIndex;
	}

	auto DSplineComponent::UpdateSplinePoint(uint32 PointIndex, const FSplinePoint& Point) -> bool
	{
		if (!SplineCurve.UpdatePoint(PointIndex, Point)) return false;
		UpdateSpline();
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::RemoveSplinePoint(uint32 PointIndex) -> bool
	{
		if (!SplineCurve.RemovePoint(PointIndex)) return false;
		UpdateSpline();
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::ClearSplinePoints() -> void
	{
		SplineCurve.ClearPoints();
		UpdateSpline();
		MarkPackageDirty();
	}

	auto DSplineComponent::SetClosedLoop(bool bClosedLoop) -> void
	{
		if (SplineCurve.IsClosedLoop() == bClosedLoop) return;
		SplineCurve.SetClosedLoop(bClosedLoop);
		UpdateSpline();
		MarkPackageDirty();
	}

	auto DSplineComponent::SetReparamStepsPerSegment(int32 Steps) -> void
	{
		const int32 ClampedSteps = std::clamp(Steps, 1, 1024);
		if (SplineCurve.GetReparamStepsPerSegment() == ClampedSteps) return;
		SplineCurve.SetReparamStepsPerSegment(ClampedSteps);
		UpdateSpline();
		MarkPackageDirty();
	}

	auto DSplineComponent::GetLocationAtParam(double Param, ESplineCoordinateSpace Space) const -> FVector3
	{
		const FVector3 LocalLocation = SplineCurve.GetLocationAtParam(Param);
		if (Space == ESplineCoordinateSpace::Local) return LocalLocation;
		return FVector3(GetComponentToWorldMatrix() * FVector4(LocalLocation, 1.0));
	}

	auto DSplineComponent::GetTangentAtParam(double Param, ESplineCoordinateSpace Space) const -> FVector3
	{
		const FVector3 LocalTangent = SplineCurve.GetTangentAtParam(Param);
		return Space == ESplineCoordinateSpace::Local ? LocalTangent : TransformTangentToWorld(LocalTangent);
	}

	auto DSplineComponent::GetDirectionAtParam(double Param, ESplineCoordinateSpace Space) const -> FVector3
	{
		return SafeNormalize(GetTangentAtParam(Param, Space));
	}

	auto DSplineComponent::GetRotationAtParam(double Param, ESplineCoordinateSpace Space) const -> FQuat
	{
		const FQuat LocalRotation = SplineCurve.GetRotationAtParam(Param);
		return Space == ESplineCoordinateSpace::Local ? LocalRotation : glm::normalize(GetWorldRotation() * LocalRotation);
	}

	auto DSplineComponent::GetScaleAtParam(double Param, ESplineCoordinateSpace Space) const -> FVector3
	{
		const FVector3 LocalScale = SplineCurve.GetScaleAtParam(Param);
		return Space == ESplineCoordinateSpace::Local ? LocalScale : GetWorldScale3D() * LocalScale;
	}

	auto DSplineComponent::GetTransformAtParam(double Param, ESplineCoordinateSpace Space) const -> FTransform
	{
		const FTransform LocalTransform = SplineCurve.GetTransformAtParam(Param);
		return Space == ESplineCoordinateSpace::Local ? LocalTransform : FTransform::Combine(GetWorldTransform(), LocalTransform);
	}

	auto DSplineComponent::GetLocationAtDistance(double Distance, ESplineCoordinateSpace Space) const -> FVector3
	{
		return GetLocationAtParam(GetParamAtDistance(Distance), Space);
	}

	auto DSplineComponent::GetTangentAtDistance(double Distance, ESplineCoordinateSpace Space) const -> FVector3
	{
		return GetTangentAtParam(GetParamAtDistance(Distance), Space);
	}

	auto DSplineComponent::GetDirectionAtDistance(double Distance, ESplineCoordinateSpace Space) const -> FVector3
	{
		return GetDirectionAtParam(GetParamAtDistance(Distance), Space);
	}

	auto DSplineComponent::GetTransformAtDistance(double Distance, ESplineCoordinateSpace Space) const -> FTransform
	{
		return GetTransformAtParam(GetParamAtDistance(Distance), Space);
	}

	auto DSplineComponent::UpdateSpline() -> void
	{
		SplineCurve.UpdateSpline();
	}

	auto DSplineComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		UpdateSpline();
		return true;
	}

	auto DSplineComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("SplineCurve")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			auto* Curve = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FSplineCurve>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			Curve->SetReparamStepsPerSegment(std::clamp(Curve->GetReparamStepsPerSegment(), 1, 1024));
		}
		return true;
	}

	auto DSplineComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("SplineCurve")
			&& (Event.Phase != EPropertyChangePhase::Committed || Event.Origin != EPropertyChangeOrigin::Edit)) UpdateSpline();
	}

	auto DSplineComponent::TransformTangentToWorld(const FVector3& LocalTangent) const -> FVector3
	{
		return GetWorldRotation() * (GetWorldScale3D() * LocalTangent);
	}
} // namespace Durin
