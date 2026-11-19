#include "Components/SplineComponent.h"

#include "DObject/Property.h"
#include "DObject/Class.h"
#include "DObject/TransactionArray.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/WeakObjectPtr.h"
#include "Math/Operations.h"
#include "Misc/ITransaction.h"

namespace Durin
{
	namespace
	{
		class FSplineTopologyTransactionAnnotation final : public ITransactionObjectAnnotation
		{
		public:
			FSplineTopologyTransactionAnnotation(DSplineComponent& InSpline, uint64 InHistoryId,
				std::shared_ptr<ITransactionObjectAnnotation> InComponentState)
				: ComponentState(std::move(InComponentState)), Spline(&InSpline), HistoryId(InHistoryId)
				, ForeignRevision(InSpline.GetForeignTopologyRevision(InHistoryId)) {}

			std::shared_ptr<ITransactionObjectAnnotation> ComponentState;

			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				DObject* Object = Spline.Get();
				Collector.AddReferencedObject(Object);
				if (ComponentState) ComponentState->AddReferencedObjects(Collector);
			}

			auto Serialize(FArchive& Archive) -> void override
			{
				Archive << HistoryId << ForeignRevision;
				if (ComponentState) ComponentState->Serialize(Archive);
			}
			auto GetAllocatedSize() const -> size_t override
			{
				return sizeof(*this) + (ComponentState ? ComponentState->GetAllocatedSize() : 0);
			}
			auto HasExpired() const noexcept -> bool override
			{
				const auto* Resolved = Spline.Get();
				return Resolved && Resolved->GetForeignTopologyRevision(HistoryId) != ForeignRevision;
			}
			auto Validate() const -> FTransactionCaptureResult override
			{
				if (!Spline.Get()) return std::unexpected("Spline transaction target is unavailable.");
				return ComponentState ? ComponentState->Validate() : FTransactionCaptureResult{};
			}

		private:
			TWeakObjectPtr<DSplineComponent> Spline;
			uint64 HistoryId;
			uint64 ForeignRevision;
		};

		auto SafeNormalize(const FVector3& Value) -> FVector3
		{
			const double LengthSquared = Math::LengthSquared(Value);
			return LengthSquared > SmallNumber ? Value / std::sqrt(LengthSquared) : FVectorConstants::Zero;
		}
	} // namespace

	DSplineComponent::DSplineComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
	}

	auto DSplineComponent::SetSplinePoints(std::vector<FSplinePoint> InPoints, std::optional<double> LoopKey) -> bool
	{
		if (bPublishingMutation) return false;
		for (const auto& Point : InPoints)
			if (!Math::IsFinite(Point.Position) || !Math::IsFinite(Point.ArriveTangent)
				|| !Math::IsFinite(Point.LeaveTangent) || !Math::IsFinite(Point.Scale)
				|| !std::isfinite(glm::dot(Point.Rotation, Point.Rotation))
				|| (Point.OutgoingInterpolation != ESplineSegmentInterpolation::Linear
					&& Point.OutgoingInterpolation != ESplineSegmentInterpolation::Cubic
					&& Point.OutgoingInterpolation != ESplineSegmentInterpolation::Constant)
				|| Point.TangentMode > ESplineTangentMode::ManualBroken)
				return false;

		FSplineCurve Candidate = SplineCurve;
		if (!Candidate.SetPoints(std::move(InPoints), LoopKey)) return false;
		if (Candidate.GetPoints() == SplineCurve.GetPoints() && Candidate.IsClosedLoop() == IsClosedLoop()
			&& Candidate.LoopKeyOverride == SplineCurve.LoopKeyOverride) return true;

		auto Evaluation = Candidate.BuildEvaluationData();
		if (!std::isfinite(Evaluation->GetLocalLength())) return false;
		const bool bRecorded = Modify(false);
		if (GUndo && HasAnyObjectFlags(EObjectFlags::Transactional) && !bRecorded) return false;
		SplineCurve = std::move(Candidate);
		PublishSplineEvaluation(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build,
			std::move(Evaluation), bRecorded ? GetRecordingHistoryId() : 0);
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::AddSplinePoint(FSplinePoint Point) -> uint32
	{
		const uint32 PointIndex = GetNumSplinePoints();
		if (!InsertSplinePoint(PointIndex, Point)) throw std::invalid_argument("Invalid spline point insertion");
		return PointIndex;
	}

	auto DSplineComponent::InsertSplinePoint(uint32 PointIndex, FSplinePoint Point) -> bool
	{
		const auto InputKey = SplineCurve.ResolveInsertionKey(PointIndex, Point.InputKey);
		if (!InputKey) return false;
		Point.InputKey = *InputKey;
		if (!EditPointRange(PointIndex, 0, {&Point, 1})) return false;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		return true;
	}

	auto DSplineComponent::DuplicateSplinePoint(uint32 PointIndex) -> std::optional<uint32>
	{
		const auto* Source = GetSplinePoint(PointIndex);
		if (!Source) return std::nullopt;
		const uint32 DuplicateIndex = PointIndex + 1;
		FSplinePoint Duplicate = *Source;
		Duplicate.InputKey = FSplinePoint::AutoInputKey;
		if (!InsertSplinePoint(DuplicateIndex, Duplicate)) return std::nullopt;
		return DuplicateIndex;
	}

	auto DSplineComponent::UpdateSplinePoint(uint32 PointIndex, FSplinePoint Point, bool bUpdateSpline) -> bool
	{
		const auto* Previous = GetSplinePoint(PointIndex);
		if (!Previous) return false;
		if (Point.InputKey == FSplinePoint::AutoInputKey) Point.InputKey = Previous->InputKey;
		if (!SplineCurve.IsValidPointKey(PointIndex, Point.InputKey)) return false;
		if (*Previous == Point || !EditPointRange(PointIndex, 1, {&Point, 1})) return false;
		if (bUpdateSpline) UpdateSpline(ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		return true;
	}

	auto DSplineComponent::RemoveSplinePoint(uint32 PointIndex) -> bool
	{
		if (PointIndex >= GetNumSplinePoints() || !EditPointRange(PointIndex, 1, {})) return false;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		return true;
	}

	auto DSplineComponent::MoveSplinePoint(uint32 FromIndex, uint32 ToIndex) -> bool
	{
		if (FromIndex >= GetNumSplinePoints() || ToIndex >= GetNumSplinePoints() || FromIndex == ToIndex) return false;
		const size_t First = std::min(FromIndex, ToIndex), Last = std::max(FromIndex, ToIndex);
		std::vector<FSplinePoint> Moved(SplineCurve.Points.begin() + First, SplineCurve.Points.begin() + Last + 1);
		if (FromIndex < ToIndex) std::rotate(Moved.begin(), Moved.begin() + 1, Moved.end());
		else std::rotate(Moved.begin(), Moved.end() - 1, Moved.end());
		for (size_t Index = 0; Index < Moved.size(); ++Index) Moved[Index].InputKey = SplineCurve.Points[First + Index].InputKey;
		if (std::equal(Moved.begin(), Moved.end(), SplineCurve.Points.begin() + First)
			|| !EditPointRange(First, Moved.size(), Moved)) return false;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		return true;
	}

	auto DSplineComponent::ClearSplinePoints() -> void
	{
		if (SplineCurve.GetNumPoints() == 0) return;
		if (!EditPointRange(0, GetNumSplinePoints(), {})) return;
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
	}

	auto DSplineComponent::SetClosedLoop(bool bClosedLoop) -> void
	{
		if (SplineCurve.IsClosedLoop() == bClosedLoop && SplineCurve.LoopKeyOverride == FSplinePoint::AutoInputKey) return;
		(void)Modify(false);
		SplineCurve.SetClosedLoop(bClosedLoop);
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
	}

	auto DSplineComponent::SetClosedLoopAtPosition(double InputKey) -> bool
	{
		if (GetNumSplinePoints() == 0 || !std::isfinite(InputKey - SplineCurve.GetPoints().back().InputKey)
			|| InputKey <= SplineCurve.GetPoints().back().InputKey) return false;
		if (IsClosedLoop() && SplineCurve.LoopKeyOverride == InputKey) return true;
		(void)Modify(false);
		SplineCurve.SetClosedLoopAtPosition(InputKey);
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
		MarkPackageDirty();
		return true;
	}

	auto DSplineComponent::EditPointRange(size_t Index, size_t RemoveCount, std::span<const FSplinePoint> Replacement) -> FTransactionCaptureResult
	{
		const auto* Root = GetClass()->FindPropertyByName("SplineCurve");
		const auto* Points = FSplineCurve::StaticStruct()->FindPropertyByName("Points");
		const std::array<const FProperty*, 1> Path{Root};
		return EditTransactionArray(*this, Points, SplineCurve.Points, Index, RemoveCount, Replacement, Path);
	}

	auto DSplineComponent::GetEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>
	{
		return std::atomic_load_explicit(&EvaluationData, std::memory_order_acquire);
	}

	auto DSplineComponent::GetScaledEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>
	{
		const auto Source = GetEvaluationData();
		const FVector3 Scale = GetWorldScale3D();
		if (Scale == FVector3(1.0)) return Source;
		const std::scoped_lock Lock(ScaledEvaluationMutex);
		if (ScaledSource != Source || EvaluationScale != Scale)
		{
			ScaledEvaluation = Source->BuildScaledDistanceData(Scale);
			ScaledSource = Source;
			EvaluationScale = Scale;
		}
		return ScaledEvaluation;
	}

	auto DSplineComponent::GetInputKeyValueAtSplinePoint(uint32 PointIndex) const -> double
	{
		const auto* Point = GetSplinePoint(PointIndex);
		return Point ? Point->InputKey : 0.0;
	}

	auto DSplineComponent::GetQuaternionAtSplineInputKey(double InputKey, ESplineCoordinateSpace Space) const -> FQuat
	{
		return GetTransformAtSplineInputKey(InputKey, Space).Rotation;
	}

	auto DSplineComponent::GetScaleAtSplineInputKey(double InputKey) const -> FVector3
	{
		return GetSampleAtSplineInputKey(InputKey).Scale;
	}

	auto DSplineComponent::GetLocationAtDistanceAlongSpline(double Distance, ESplineCoordinateSpace Space) const -> FVector3
	{
		return GetLocationAtSplineInputKey(GetInputKeyValueAtDistanceAlongSpline(Distance), Space);
	}

	auto DSplineComponent::GetLocationAtTime(double Time, ESplineCoordinateSpace Space, bool bUseConstantVelocity) const -> FVector3
	{
		if (bUseConstantVelocity)
			return GetLocationAtDistanceAlongSpline(Duration > 0.0 ? std::clamp(Time / Duration, 0.0, 1.0) * GetSplineLength() : 0.0, Space);
		return GetLocationAtSplineInputKey(GetInputKeyValueAtTime(Time), Space);
	}

	auto DSplineComponent::GetTimeAtDistanceAlongSpline(double Distance) const -> double
	{
		const auto Data = GetEvaluationData();
		if (Data->GetNumSegments() == 0 || !(Duration > 0.0)) return 0.0;
		const double FirstKey = Data->GetSegments().front().StartKey;
		const double LastKey = Data->GetSegments().back().EndKey;
		return Duration * (GetInputKeyValueAtDistanceAlongSpline(Distance) - FirstKey) / (LastKey - FirstKey);
	}

	auto DSplineComponent::GetSampleAtSplineInputKey(double InputKey, ESplineCoordinateSpace Space) const -> FSplineSample
	{
		const FSplineSample Sample = GetEvaluationData()->EvaluateAtInputKey(InputKey);
		return Space == ESplineCoordinateSpace::Local ? Sample : TransformSampleToWorld(Sample);
	}

	auto DSplineComponent::GetLocationAtSplineInputKey(double InputKey, ESplineCoordinateSpace Space) const -> FVector3
	{
		return GetSampleAtSplineInputKey(InputKey, Space).Position;
	}

	auto DSplineComponent::GetTangentAtSplineInputKey(double InputKey, ESplineCoordinateSpace Space) const -> FVector3
	{
		return GetSampleAtSplineInputKey(InputKey, Space).FirstDerivative;
	}

	auto DSplineComponent::GetTransformAtSplineInputKey(double InputKey, ESplineCoordinateSpace Space, bool bUseScale) const -> FTransform
	{
		const FSplineSample Sample = GetSampleAtSplineInputKey(InputKey);
		const FVector3 Forward = Math::NormalizeOr(Sample.Direction, FVector3(1.0, 0.0, 0.0));
		const FVector3 Up = Sample.Rotation * DefaultUpVector;
		const FVector3 Fallback = std::abs(Forward.z) < 0.9 ? FVector3(0.0, 0.0, 1.0) : FVector3(0.0, 1.0, 0.0);
		const FVector3 Right = Math::NormalizeOr(Math::Cross(Up, Forward), SafeNormalize(Math::Cross(Fallback, Forward)));
		FTransform Transform;
		Transform.Translation = Sample.Position;
		Transform.Rotation = Math::QuaternionFromMatrix(FMatrix(
			FVector4(Forward, 0.0), FVector4(Right, 0.0), FVector4(Math::Cross(Forward, Right), 0.0), FVector4(0.0, 0.0, 0.0, 1.0)));
		if (bUseScale) Transform.Scale3D = Sample.Scale;
		return Space == ESplineCoordinateSpace::Local ? Transform : FTransform::Combine(GetWorldTransform(), Transform);
	}

	auto DSplineComponent::GetTransformAtDistanceAlongSpline(double Distance, ESplineCoordinateSpace Space, bool bUseScale) const -> FTransform
	{
		return GetTransformAtSplineInputKey(GetInputKeyValueAtDistanceAlongSpline(Distance), Space, bUseScale);
	}

	auto DSplineComponent::GetTransformAtTime(double Time, ESplineCoordinateSpace Space, bool bUseConstantVelocity, bool bUseScale) const -> FTransform
	{
		if (bUseConstantVelocity)
			return GetTransformAtDistanceAlongSpline(Duration > 0.0 ? std::clamp(Time / Duration, 0.0, 1.0) * GetSplineLength() : 0.0,
				Space, bUseScale);
		return GetTransformAtSplineInputKey(GetInputKeyValueAtTime(Time), Space, bUseScale);
	}

	auto DSplineComponent::FindInputKeyClosestToWorldLocation(const FVector3& Position) const -> double
	{
		return GetEvaluationData()->GetInputKeyAtParameter(FindNearestParameter(Position, ESplineCoordinateSpace::World));
	}

	auto DSplineComponent::GetInputKeyValueAtDistanceAlongSpline(double Distance) const -> double
	{
		const auto Data = GetScaledEvaluationData();
		if (Distance >= Data->GetLocalLength() && Data->GetNumSegments() > 0)
			return Data->GetSegments().back().EndKey;
		return Data->GetInputKeyAtParameter(Data->GetParameterAtLocalDistance(std::max(0.0, Distance)));
	}

	auto DSplineComponent::GetDistanceAlongSplineAtSplineInputKey(double InputKey) const -> double
	{
		const auto Data = GetScaledEvaluationData();
		return Data->GetLocalDistanceAtParameter(Data->GetParameterAtInputKey(InputKey));
	}

	auto DSplineComponent::GetInputKeyValueAtTime(double Time) const -> double
	{
		const auto Data = GetEvaluationData();
		const double Start = Data->GetInputKeyAtParameter({});
		if (Data->GetNumSegments() == 0 || !(Duration > 0.0)) return Start;
		return std::lerp(Start, Data->GetSegments().back().EndKey, std::clamp(Time / Duration, 0.0, 1.0));
	}

	auto DSplineComponent::GetSplineLength() const -> double
	{
		return GetScaledEvaluationData()->GetLocalLength();
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

	auto DSplineComponent::GetRecordingHistoryId() const -> uint64
	{
		return GUndo && HasAnyObjectFlags(EObjectFlags::Transactional) && GUndo->ContainsObject(this)
			? GUndo->GetContext().HistoryId : 0;
	}

	auto DSplineComponent::UpdateSpline(ESplineChangeFlags ChangeFlags) -> void
	{
		UpdateSplineForHistory(ChangeFlags, GetRecordingHistoryId());
	}

	auto DSplineComponent::UpdateSplineForHistory(ESplineChangeFlags Flags, uint64 HistoryId) -> void
	{
		checkf(!bPublishingMutation, "Spline mutation listeners must not mutate the publishing Spline component reentrantly.");
		if (SplineCurve.RepairInputKeys()) Flags |= ESplineChangeFlags::Geometry;
		PublishSplineEvaluation(Flags, SplineCurve.BuildEvaluationData(), HistoryId);
	}

	auto DSplineComponent::PublishSplineEvaluation(ESplineChangeFlags ChangeFlags,
		std::shared_ptr<const FSplineEvaluationData> PublishedEvaluation, uint64 HistoryId) -> void
	{
		std::atomic_store_explicit(&EvaluationData, PublishedEvaluation, std::memory_order_release);
		++SplineRevision;
		if (EnumHasAnyFlags(ChangeFlags, ESplineChangeFlags::Topology))
		{
			if (TopologyHistoryId != HistoryId) PreviousHistoryTopologyRevision = SplineTopologyRevision;
			TopologyHistoryId = HistoryId;
			++SplineTopologyRevision;
		}
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

	auto DSplineComponent::FindOrCreateTransactionAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		const uint64 HistoryId = GUndo ? GUndo->GetContext().HistoryId : FScopedTransactionHistoryReplay::GetHistoryId();
		auto ComponentState = Super::FindOrCreateTransactionAnnotation();
		if (HistoryId == 0) return ComponentState;
		return std::make_shared<FSplineTopologyTransactionAnnotation>(*const_cast<DSplineComponent*>(this), HistoryId,
			std::move(ComponentState));
	}

	auto DSplineComponent::PostEditUndo(std::shared_ptr<ITransactionObjectAnnotation> Annotation) -> void
	{
		if (const auto SplineState = std::dynamic_pointer_cast<FSplineTopologyTransactionAnnotation>(Annotation))
			Annotation = SplineState->ComponentState;
		Super::PostEditUndo(std::move(Annotation));
	}

	auto DSplineComponent::PostLoad() -> void
	{
		Super::PostLoad();
		UpdateSpline(ESplineChangeFlags::Topology | ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
	}

	auto DSplineComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		return Super::PreEditChangeProperty(Proposal);
	}

	auto DSplineComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("SplineCurve"))
		{
			ESplineChangeFlags Flags = ESplineChangeFlags::Geometry | ESplineChangeFlags::Build;
			const FProperty* Points = FSplineCurve::StaticStruct()->FindPropertyByName("Points");
			const FProperty* ClosedLoop = FSplineCurve::StaticStruct()->FindPropertyByName("bClosedLoop");
			if (Event.Kind != EPropertyChangeKind::ValueSet || !Event.LeafProperty
				|| Event.LeafProperty == Event.MemberProperty || Event.LeafProperty == Points || Event.LeafProperty == ClosedLoop)
				Flags |= ESplineChangeFlags::Topology;
			UpdateSplineForHistory(Flags, Event.Origin == EPropertyChangeOrigin::Edit
				? GetRecordingHistoryId() : FScopedTransactionHistoryReplay::GetHistoryId());
		}
	}

	auto DSplineComponent::TransformSampleToWorld(const FSplineSample& LocalSample) const -> FSplineSample
	{
		FSplineSample Result = LocalSample;
		Result.Position = FVector3(GetComponentToWorldMatrix() * FVector4(LocalSample.Position, 1.0));
		Result.FirstDerivative = GetWorldRotation() * (GetWorldScale3D() * LocalSample.FirstDerivative);
		Result.SecondDerivative = GetWorldRotation() * (GetWorldScale3D() * LocalSample.SecondDerivative);
		Result.Direction = SafeNormalize(Result.FirstDerivative);
		Result.Rotation = GetWorldRotation() * LocalSample.Rotation;
		return Result;
	}
} // namespace Durin
