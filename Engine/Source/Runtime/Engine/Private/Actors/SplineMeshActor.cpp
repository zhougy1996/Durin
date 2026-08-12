#include "Actors/SplineMeshActor.h"

#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "DObject/Property.h"
#include "Engine/ActorConstruction.h"
#include "Materials/MaterialInterface.h"
#include "Spline/SplineMeshDeformer.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	namespace
	{
		auto RotateAround(const FVector3& Value, const FVector3& Axis, double Angle) -> FVector3
		{
			return Value * std::cos(Angle) + Math::Cross(Axis, Value) * std::sin(Angle)
				+ Axis * Math::Dot(Axis, Value) * (1.0 - std::cos(Angle));
		}

		auto SignedRoll(const FVector3& FromUp, const FVector3& ToUp,
			const FVector3& Forward) -> double
		{
			return std::atan2(Math::Dot(Forward, Math::Cross(FromUp, ToUp)),
				Math::Dot(FromUp, ToUp));
		}
	}

	ASplineMeshActor::ASplineMeshActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SplineComponent = CreateDefaultComponent<DSplineComponent>("SplineComponent");
		SetRootComponent(SplineComponent);
		SplineMutationListenerId = SplineComponent->AddSplineMutationListener(
			[this](uint64, ESplineChangeFlags, std::shared_ptr<const FSplineEvaluationData>) {
				if (!IsBeingDestroyed()) RequestNativeReconstruction();
			});
	}

	auto ASplineMeshActor::SetPathMesh(DStaticMesh* Mesh) -> void
	{
		if (PathMesh == Mesh) return;
		PathMesh = Mesh;
		MarkPackageDirty();
		RequestNativeReconstruction();
	}

	auto ASplineMeshActor::SetPathMaterial(DMaterialInterface* Material) -> void
	{
		if (PathMaterial == Material) return;
		PathMaterial = Material;
		MarkPackageDirty();
		RequestNativeReconstruction();
	}

	auto ASplineMeshActor::SetPathCollisionEnabled(bool bEnabled) -> void
	{
		if (bEnablePathCollision == bEnabled) return;
		bEnablePathCollision = bEnabled;
		MarkPackageDirty();
		RequestNativeReconstruction();
	}

	auto ASplineMeshActor::SetPathVisible(bool bVisible) -> void
	{
		if (bPathVisible == bVisible) return;
		bPathVisible = bVisible;
		MarkPackageDirty();
		RequestNativeReconstruction();
	}

	auto ASplineMeshActor::BeginDestroy() -> void
	{
		if (SplineComponent && SplineMutationListenerId != 0)
			SplineComponent->RemoveSplineMutationListener(SplineMutationListenerId);
		SplineMutationListenerId = 0;
		Super::BeginDestroy();
	}

	auto ASplineMeshActor::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate != FName("SplineComponent"))
			RequestNativeReconstruction();
	}

	auto ASplineMeshActor::OnNativeConstruct(FActorConstructionContext& Context,
		std::string& OutError) -> bool
	{
		if (!Super::OnNativeConstruct(Context, OutError)) return false;
		if (!SplineComponent || !PathMesh)
		{
			OutError.clear();
			return true;
		}
		const auto Evaluation = SplineComponent->GetEvaluationData();
		const auto& Points = SplineComponent->GetSplinePoints();
		if (!Evaluation || Evaluation->GetNumSegments() == 0 || Points.empty())
		{
			OutError.clear();
			return true;
		}

		const FSplinePathFrameData PathFrames = FSplinePathFrameData::Build(*Evaluation, FrameUpDirection);
		std::vector<FSplineMeshFrame> EndpointFrames(Evaluation->GetNumSegments() + 1);
		std::vector<bool> EndpointFrameSet(EndpointFrames.size(), false);
		for (const FSplinePathFrameSample& Sample : PathFrames.GetSamples())
		{
			if (Sample.Parameter.T <= 1.e-9)
			{
				EndpointFrames[Sample.Parameter.SegmentIndex] = Sample.Frame;
				EndpointFrameSet[Sample.Parameter.SegmentIndex] = true;
			}
			if (Sample.Parameter.T >= 1.0 - 1.e-9)
			{
				const size_t EndpointIndex = static_cast<size_t>(Sample.Parameter.SegmentIndex) + 1;
				EndpointFrames[EndpointIndex] = Sample.Frame;
				EndpointFrameSet[EndpointIndex] = true;
			}
		}
		for (size_t EndpointIndex = 1; EndpointIndex < EndpointFrames.size(); ++EndpointIndex)
			if (!EndpointFrameSet[EndpointIndex] && EndpointFrameSet[EndpointIndex - 1])
				EndpointFrames[EndpointIndex] = EndpointFrames[EndpointIndex - 1];

		struct FSegmentSpec
		{
			FActorGeneratedComponentKey Key;
			FSplineMeshParams Params;
		};
		std::vector<FSegmentSpec> Specs;
		Specs.reserve(Evaluation->GetNumSegments());
		for (uint32 SegmentIndex = 0; SegmentIndex < Evaluation->GetNumSegments(); ++SegmentIndex)
		{
			const FSplineSample Start = Evaluation->Evaluate({SegmentIndex, 0.0});
			const FSplineSample End = Evaluation->Evaluate({SegmentIndex, 1.0});
			FSplineMeshParams Params;
			Params.StartPosition = Start.Position;
			Params.StartTangent = Start.FirstDerivative;
			Params.EndPosition = End.Position;
			Params.EndTangent = End.FirstDerivative;
			Params.StartScale = PathScale;
			Params.EndScale = PathScale;
			Params.StartRollRadians = PathRollRadians;
			Params.EndRollRadians = 0.0;
			Params.StartOffset = PathOffset;
			Params.EndOffset = PathOffset;
			Params.SplineUpDirection = EndpointFrameSet[SegmentIndex]
				? EndpointFrames[SegmentIndex].Up : FrameUpDirection;
			Params.ForwardAxis = ForwardAxis;
			Params.Interpolation = PathInterpolation;
			if (EndpointFrameSet[SegmentIndex + 1])
			{
				const FSplineMeshSample BaseEnd = FSplineMeshDeformer::Evaluate(Params, 1.0);
				const FVector3 TargetUp = RotateAround(EndpointFrames[SegmentIndex + 1].Up,
					EndpointFrames[SegmentIndex + 1].Forward, PathRollRadians);
				Params.EndRollRadians = SignedRoll(BaseEnd.Frame.Up, TargetUp, BaseEnd.Frame.Forward);
			}
			const uint32 StartPointIndex = SegmentIndex % static_cast<uint32>(Points.size());
			Specs.push_back({{FName("SplineSegment"), Points[StartPointIndex].Id}, Params});
		}

		for (uint32 SegmentIndex = 0; SegmentIndex < Specs.size(); ++SegmentIndex)
		{
			auto* Component = Cast<DSplineMeshComponent>(Context.AcquireGeneratedComponent(
				Specs[SegmentIndex].Key, DSplineMeshComponent::StaticClass(),
				FName(std::format("SplineMeshSegment{}", SegmentIndex))));
			if (!Component)
			{
				OutError = Context.GetError();
				return false;
			}
			Component->SetSplineMeshCollisionMode(bEnablePathCollision
				? ESplineMeshCollisionMode::DeformedTriangleMesh : ESplineMeshCollisionMode::Disabled);
			Component->SetStaticMesh(PathMesh.Get());
			if (!Component->SetSplineMeshParams(Specs[SegmentIndex].Params, &OutError)) return false;
			Component->ClearMaterialOverrides();
			if (PathMaterial && !Component->SetMaterial(PathMaterial.Get()))
			{
				OutError = "SplineMesh path material could not bind to source slot 0.";
				return false;
			}
			Component->SetCollisionEnabled(bEnablePathCollision
				? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
			Component->SetVisible(bPathVisible);
			if (Component->GetAttachParent() != SplineComponent.Get()
				&& !Component->AttachToComponent(SplineComponent.Get(), EAttachmentTransformRule::SnapToTarget))
			{
				OutError = "Generated SplineMesh segment could not attach to the Spline root.";
				return false;
			}
		}
		OutError.clear();
		return true;
	}
}
