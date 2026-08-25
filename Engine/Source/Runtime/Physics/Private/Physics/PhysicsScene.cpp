#include "Physics/PhysicsScene.h"

#include "Collision/CollisionGeometry.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	namespace
	{
		enum EHitDifference : uint32
		{
			HitDifferenceStatus = 1u << 0,
			HitDifferenceCount = 1u << 1,
			HitDifferenceOrder = 1u << 2,
			HitDifferenceHandle = 1u << 3,
			HitDifferenceResponse = 1u << 4,
			HitDifferenceTime = 1u << 5,
			HitDifferenceDistance = 1u << 6,
			HitDifferenceLocation = 1u << 7,
			HitDifferenceImpactPoint = 1u << 8,
			HitDifferenceImpactNormal = 1u << 9,
			HitDifferencePenetrationDepth = 1u << 10,
			HitDifferenceUserToken = 1u << 11,
			HitDifferenceStartPenetrating = 1u << 12
		};

		auto IsIgnored(FPhysicsActorHandle Handle, const FPhysicsQueryFilter& Filter) -> bool
		{
			return std::ranges::find(Filter.IgnoredActors, Handle) != Filter.IgnoredActors.end();
		}

		auto ResolveResponse(const FPhysicsFilterData& Body, const FPhysicsQueryFilter& Query)
			-> EPhysicsQueryResponse
		{
			if (Query.QueryChannel >= MaximumPhysicsChannels || Body.ObjectChannel >= MaximumPhysicsChannels)
				return EPhysicsQueryResponse::Ignore;
			const EPhysicsQueryResponse BodyResponse = Body.Responses[Query.QueryChannel];
			const EPhysicsQueryResponse QueryResponse = Query.Responses[Body.ObjectChannel];
			if (BodyResponse == EPhysicsQueryResponse::Ignore || QueryResponse == EPhysicsQueryResponse::Ignore)
				return EPhysicsQueryResponse::Ignore;
			if (BodyResponse == EPhysicsQueryResponse::Overlap || QueryResponse == EPhysicsQueryResponse::Overlap)
				return EPhysicsQueryResponse::Overlap;
			return EPhysicsQueryResponse::Block;
		}

		auto IsCloser(const FPhysicsQueryHit& Candidate, const FPhysicsQueryHit& Current) -> bool
		{
			return !Current.IsHit() || Candidate.Time < Current.Time
				|| (Candidate.Time == Current.Time && Candidate.ActorHandle < Current.ActorHandle);
		}

		auto SortMultiResults(std::vector<FPhysicsQueryHit>& Hits) -> void
		{
			std::ranges::sort(Hits, {}, &FPhysicsQueryHit::ActorHandle);
		}

		auto IsNear(double Left, double Right, double Tolerance) -> bool
		{
			return std::isfinite(Left) && std::isfinite(Right) && std::abs(Left - Right) <= Tolerance;
		}

		auto IsVectorNear(const FVector3& Left, const FVector3& Right, double Tolerance) -> bool
		{
			return IsNear(Left.x, Right.x, Tolerance)
				&& IsNear(Left.y, Right.y, Tolerance)
				&& IsNear(Left.z, Right.z, Tolerance);
		}

		auto BuildHitDifference(const FPhysicsQueryHit& Reference, const FPhysicsQueryHit& Production) -> uint32
		{
			uint32 Difference = 0;
			if (Reference.ActorHandle != Production.ActorHandle) Difference |= HitDifferenceHandle;
			if (Reference.Response != Production.Response) Difference |= HitDifferenceResponse;
			if (!IsNear(Reference.Time, Production.Time, 1.0e-12)) Difference |= HitDifferenceTime;
			if (!IsNear(Reference.Distance, Production.Distance, 1.0e-8)) Difference |= HitDifferenceDistance;
			if (!IsVectorNear(Reference.Location, Production.Location, 1.0e-8)) Difference |= HitDifferenceLocation;
			if (!IsVectorNear(Reference.ImpactPoint, Production.ImpactPoint, 1.0e-8))
				Difference |= HitDifferenceImpactPoint;
			if (!IsVectorNear(Reference.ImpactNormal, Production.ImpactNormal, 1.0e-8))
				Difference |= HitDifferenceImpactNormal;
			if (!IsNear(Reference.PenetrationDepth, Production.PenetrationDepth, 1.0e-8))
				Difference |= HitDifferencePenetrationDepth;
			if (Reference.UserToken != Production.UserToken) Difference |= HitDifferenceUserToken;
			if (Reference.bStartPenetrating != Production.bStartPenetrating)
				Difference |= HitDifferenceStartPenetrating;
			return Difference;
		}

		auto BoundedCount(size_t Count) -> uint32
		{
			return static_cast<uint32>(std::min<size_t>(Count, std::numeric_limits<uint32>::max()));
		}

		constexpr uint32 InvalidSpatialIndex = std::numeric_limits<uint32>::max();
		constexpr uint32 SpatialLeafBit = 1u << 31;
		constexpr double BoundsPadding = 1.0e-7;

		auto UnionBounds(const std::array<float, 6>& Left, const std::array<float, 6>& Right)
			-> std::array<float, 6>
		{
			return {std::min(Left[0], Right[0]), std::min(Left[1], Right[1]), std::min(Left[2], Right[2]),
				std::max(Left[3], Right[3]), std::max(Left[4], Right[4]), std::max(Left[5], Right[5])};
		}

		auto BoundsSurfaceArea(const std::array<float, 6>& Bounds) -> double
		{
			const double X = static_cast<double>(Bounds[3]) - Bounds[0];
			const double Y = static_cast<double>(Bounds[4]) - Bounds[1];
			const double Z = static_cast<double>(Bounds[5]) - Bounds[2];
			return 2.0 * (X * Y + Y * Z + Z * X);
		}

		auto ContainsBounds(const std::array<float, 6>& Outer, const std::array<float, 6>& Inner) -> bool
		{
			return Inner[0] >= Outer[0] && Inner[1] >= Outer[1] && Inner[2] >= Outer[2]
				&& Inner[3] <= Outer[3] && Inner[4] <= Outer[4] && Inner[5] <= Outer[5];
		}

		auto ExpandMovingBounds(std::array<float, 6>& Bounds) -> void
		{
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				const float Margin = std::max((Bounds[Axis + 3] - Bounds[Axis]) * 0.1f, 0.01f);
				Bounds[Axis] = std::nextafter(Bounds[Axis] - Margin, -std::numeric_limits<float>::infinity());
				Bounds[Axis + 3] = std::nextafter(
					Bounds[Axis + 3] + Margin, std::numeric_limits<float>::infinity());
			}
		}

		auto MovingProxyBounds(const std::array<float, 6>& ExactBounds) -> std::array<float, 6>
		{
			std::array<float, 6> Result = ExactBounds;
			ExpandMovingBounds(Result);
			return Result;
		}

		auto CompactBounds(const FVector3& Min, const FVector3& Max, std::array<float, 6>& OutBounds) -> bool
		{
			if (!Math::IsFinite(Min) || !Math::IsFinite(Max)
				|| Min.x > Max.x || Min.y > Max.y || Min.z > Max.z) return false;
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				const float Lower = static_cast<float>(Min[Axis] - BoundsPadding);
				const float Upper = static_cast<float>(Max[Axis] + BoundsPadding);
				if (!std::isfinite(Lower) || !std::isfinite(Upper)) return false;
				OutBounds[Axis] = std::nextafter(Lower, -std::numeric_limits<float>::infinity());
				OutBounds[Axis + 3] = std::nextafter(Upper, std::numeric_limits<float>::infinity());
			}
			return true;
		}

		auto BuildShapeBounds(const FCollisionShape& Shape, const FTransform& Transform,
			FVector3& OutMin, FVector3& OutMax) -> bool
		{
			if (!Shape.IsValid() || !IsValidPhysicsTransform(Transform)) return false;
			FQuat Rotation;
			if (!Math::TryNormalize(Transform.Rotation, Rotation)) return false;
			FVector3 Extent{0.0};
			switch (Shape.GetType())
			{
			case ECollisionShapeType::Box:
			{
				const FVector3 Local = Shape.GetBoxHalfExtent() * Transform.Scale3D;
				const FVector3 X = Math::Abs(Math::RotateVector(Rotation, {Local.x, 0.0, 0.0}));
				const FVector3 Y = Math::Abs(Math::RotateVector(Rotation, {0.0, Local.y, 0.0}));
				const FVector3 Z = Math::Abs(Math::RotateVector(Rotation, {0.0, 0.0, Local.z}));
				Extent = X + Y + Z;
				break;
			}
			case ECollisionShapeType::Sphere:
			{
				const double Radius = Shape.GetSphereRadius()
					* std::max({Transform.Scale3D.x, Transform.Scale3D.y, Transform.Scale3D.z});
				Extent = FVector3(Radius);
				break;
			}
			case ECollisionShapeType::Capsule:
			{
				const double Radius = Shape.GetCapsuleRadius()
					* std::max(Transform.Scale3D.x, Transform.Scale3D.y);
				const double HalfHeight = std::max(Radius,
					Shape.GetCapsuleHalfHeight() * Transform.Scale3D.z);
				const FVector3 Axis = Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Up));
				Extent = Axis * (HalfHeight - Radius) + FVector3(Radius);
				break;
			}
			}
			OutMin = Transform.Translation - Extent;
			OutMax = Transform.Translation + Extent;
			return Math::IsFinite(OutMin) && Math::IsFinite(OutMax);
		}

		auto HasSameSpatialState(const FPhysicsBodyDesc& Left, const FPhysicsBodyDesc& Right) -> bool
		{
			if (Left.MotionType != Right.MotionType
				|| Left.Geometry.GetIdentity() != Right.Geometry.GetIdentity()) return false;
			for (uint32 Index = 0; Index < 4; ++Index)
				if (Left.Transform.Rotation[Index] != Right.Transform.Rotation[Index]) return false;
			for (uint32 Index = 0; Index < 3; ++Index)
			{
				if (Left.Transform.Translation[Index] != Right.Transform.Translation[Index]
					|| Left.Transform.Scale3D[Index] != Right.Transform.Scale3D[Index]) return false;
			}
			return true;
		}

	}

	FPhysicsScene::FPhysicsScene()
		: OwningThread(std::this_thread::get_id())
	{
	}

	FPhysicsScene::~FPhysicsScene() = default;

	auto FPhysicsScene::AddBody(const FPhysicsBodyDesc& Desc) -> FPhysicsActorHandle
	{
		CountMutation(Diagnostics.Mutations.AddCalls);
		if (!IsOwningThread() || !Desc.Geometry.IsValid() || !IsValidPhysicsTransform(Desc.Transform)
			|| Desc.Filter.ObjectChannel >= MaximumPhysicsChannels || !IsValidMotionType(Desc.MotionType))
		{
			CountMutation(Diagnostics.Mutations.AddRejected);
			return {};
		}
		std::array<float, 6> Bounds;
		if (!BuildBodyBounds(Desc, Bounds) || Slots.size() >= SpatialLeafBit)
		{
			CountMutation(Diagnostics.Mutations.AddRejected);
			return {};
		}
		CountMutation(Diagnostics.Mutations.BoundBuilds);
		uint32 SlotIndex = InvalidSpatialIndex;
		if (FreeSlot != InvalidSpatialIndex)
		{
			SlotIndex = FreeSlot;
			FSlot& Slot = Slots[SlotIndex];
			FreeSlot = Slot.DenseOrNext;
			CountMutation(Diagnostics.Mutations.SlotReuses);
		}
		else
		{
			if (Slots.size() == Slots.capacity())
			{
				const size_t Capacity = ((Slots.size() + 256) / 256) * 256;
				Slots.reserve(Capacity);
				Bodies.reserve(Capacity);
			}
			SlotIndex = static_cast<uint32>(Slots.size());
			Slots.push_back({});
		}
		FSlot& Slot = Slots[SlotIndex];
		Slot.DenseOrNext = static_cast<uint32>(Bodies.size());
		Slot.ProxyParent = InvalidSpatialIndex;
		Bodies.push_back({Desc, Bounds, SlotIndex});
		RetainGeometry(Desc.Geometry);
		if (Desc.MotionType == EPhysicsBodyMotionType::Static) bStaticTreeDirty = true;
		else InsertMovingProxy(SlotIndex);
		if (Desc.MotionType == EPhysicsBodyMotionType::Static) ++Diagnostics.Mutations.StaticBodies;
		else if (Desc.MotionType == EPhysicsBodyMotionType::Kinematic) ++Diagnostics.Mutations.KinematicBodies;
		else ++Diagnostics.Mutations.DynamicBodies;
		const FPhysicsActorHandle Handle{static_cast<uint64>(SlotIndex) + 1, Slot.Generation};
		CountMutation(Diagnostics.Mutations.AddSuccesses);
		Diagnostics.Mutations.BodiesPresent = static_cast<uint64>(Bodies.size());
		RefreshSpatialDiagnostics();
		return Handle;
	}

	auto FPhysicsScene::RemoveBody(FPhysicsActorHandle Handle) -> bool
	{
		CountMutation(Diagnostics.Mutations.RemoveCalls);
		if (!IsOwningThread())
		{
			CountMutation(Diagnostics.Mutations.RemoveRejected);
			return false;
		}
		FBodyRecord* Body = FindBody(Handle);
		if (!Body)
		{
			CountMutation(Diagnostics.Mutations.RemoveRejected);
			CountMutation(Diagnostics.Mutations.FailedLookups);
			return false;
		}
		const uint32 SlotIndex = Body->Slot;
		const uint32 DenseIndex = Slots[SlotIndex].DenseOrNext;
		const FCollisionGeometryRef RemovedGeometry = Body->Desc.Geometry;
		if (Body->Desc.MotionType == EPhysicsBodyMotionType::Static) bStaticTreeDirty = true;
		else RemoveMovingProxy(SlotIndex);
		if (Body->Desc.MotionType == EPhysicsBodyMotionType::Static) --Diagnostics.Mutations.StaticBodies;
		else if (Body->Desc.MotionType == EPhysicsBodyMotionType::Kinematic) --Diagnostics.Mutations.KinematicBodies;
		else --Diagnostics.Mutations.DynamicBodies;
		if (DenseIndex + 1 != Bodies.size())
		{
			Bodies[DenseIndex] = std::move(Bodies.back());
			Slots[Bodies[DenseIndex].Slot].DenseOrNext = DenseIndex;
			CountMutation(Diagnostics.Mutations.DenseSwaps);
		}
		Bodies.pop_back();
		ReleaseGeometry(RemovedGeometry);
		FSlot& Slot = Slots[SlotIndex];
		Slot.ProxyParent = InvalidSpatialIndex;
		if (Slot.Generation != std::numeric_limits<uint32>::max())
		{
			++Slot.Generation;
			Slot.DenseOrNext = FreeSlot;
			FreeSlot = SlotIndex;
		}
		else Slot.DenseOrNext = InvalidSpatialIndex;
		CountMutation(Diagnostics.Mutations.RemoveSuccesses);
		Diagnostics.Mutations.BodiesPresent = static_cast<uint64>(Bodies.size());
		RefreshSpatialDiagnostics();
		return true;
	}

	auto FPhysicsScene::UpdateBody(FPhysicsActorHandle Handle, const FPhysicsBodyDesc& Desc) -> bool
	{
		CountMutation(Diagnostics.Mutations.UpdateCalls);
		if (!IsOwningThread() || Desc.Filter.ObjectChannel >= MaximumPhysicsChannels
			|| !IsValidMotionType(Desc.MotionType))
		{
			CountMutation(Diagnostics.Mutations.UpdateRejected);
			return false;
		}
		FBodyRecord* Body = FindBody(Handle);
		if (!Body)
		{
			CountMutation(Diagnostics.Mutations.UpdateRejected);
			CountMutation(Diagnostics.Mutations.FailedLookups);
			return false;
		}
		if (HasSameSpatialState(Body->Desc, Desc))
		{
			Body->Desc = Desc;
			CountMutation(Diagnostics.Mutations.FilterOnlyUpdates);
			CountMutation(Diagnostics.Mutations.UpdateSuccesses);
			return true;
		}
		if (!Desc.Geometry.IsValid() || !IsValidPhysicsTransform(Desc.Transform))
		{
			CountMutation(Diagnostics.Mutations.UpdateRejected);
			return false;
		}
		std::array<float, 6> Bounds;
		if (!BuildBodyBounds(Desc, Bounds))
		{
			CountMutation(Diagnostics.Mutations.UpdateRejected);
			return false;
		}
		CountMutation(Diagnostics.Mutations.BoundBuilds);
		const EPhysicsBodyMotionType OldMotion = Body->Desc.MotionType;
		const FCollisionGeometryRef OldGeometry = Body->Desc.Geometry;
		const bool bMigration = OldMotion != Desc.MotionType;
		const bool bContainedMovingUpdate = !bMigration && OldMotion != EPhysicsBodyMotionType::Static
			&& ContainsBounds(MovingProxyBounds(Body->Bounds), Bounds);
		const bool bSpatialChange = !bContainedMovingUpdate && Body->Bounds != Bounds;
		if (bMigration || bSpatialChange)
		{
			if (OldMotion == EPhysicsBodyMotionType::Static) bStaticTreeDirty = true;
			else RemoveMovingProxy(Body->Slot);
		}
		else if (bContainedMovingUpdate) CountMutation(Diagnostics.Mutations.MovingContainedUpdates);
		Body->Desc = Desc;
		if (OldGeometry.GetIdentity() != Desc.Geometry.GetIdentity())
		{
			RetainGeometry(Desc.Geometry);
			ReleaseGeometry(OldGeometry);
		}
		Body->Bounds = Bounds;
		if (bMigration || bSpatialChange)
		{
			if (Desc.MotionType == EPhysicsBodyMotionType::Static) bStaticTreeDirty = true;
			else
			{
				InsertMovingProxy(Body->Slot);
				if (bSpatialChange && !bMigration) CountMutation(Diagnostics.Mutations.MovingReinsertions);
			}
		}
		if (bMigration) CountMutation(Diagnostics.Mutations.MotionMigrations);
		if (bMigration)
		{
			if (OldMotion == EPhysicsBodyMotionType::Static) --Diagnostics.Mutations.StaticBodies;
			else if (OldMotion == EPhysicsBodyMotionType::Kinematic) --Diagnostics.Mutations.KinematicBodies;
			else --Diagnostics.Mutations.DynamicBodies;
			if (Desc.MotionType == EPhysicsBodyMotionType::Static) ++Diagnostics.Mutations.StaticBodies;
			else if (Desc.MotionType == EPhysicsBodyMotionType::Kinematic) ++Diagnostics.Mutations.KinematicBodies;
			else ++Diagnostics.Mutations.DynamicBodies;
		}
		CountMutation(Diagnostics.Mutations.UpdateSuccesses);
		return true;
	}

	auto FPhysicsScene::ContainsBody(FPhysicsActorHandle Handle) const -> bool
	{
		return IsOwningThread() && FindBody(Handle) != nullptr;
	}

	auto FPhysicsScene::GetBodyCount() const -> size_t
	{
		return IsOwningThread() ? Bodies.size() : 0;
	}

	auto FPhysicsScene::RetainGeometry(const FCollisionGeometryRef& Geometry) -> void
	{
		auto [Entry, bInserted] = GeometryResources.try_emplace(
			Geometry.GetIdentity(), std::pair<uint64, uint64>{0, Geometry.GetRetainedBytes()});
		++Entry->second.first;
		if (bInserted)
		{
			Diagnostics.Mutations.UniqueGeometryResources = static_cast<uint64>(GeometryResources.size());
			SaturatingAdd(Diagnostics.Mutations.RetainedGeometryBytes, Entry->second.second);
		}
	}

	auto FPhysicsScene::ReleaseGeometry(const FCollisionGeometryRef& Geometry) -> void
	{
		const auto Entry = GeometryResources.find(Geometry.GetIdentity());
		if (Entry == GeometryResources.end()) return;
		if (--Entry->second.first != 0) return;
		Diagnostics.Mutations.RetainedGeometryBytes -= Entry->second.second;
		GeometryResources.erase(Entry);
		Diagnostics.Mutations.UniqueGeometryResources = static_cast<uint64>(GeometryResources.size());
	}

	auto FPhysicsScene::CaptureBodies() const -> std::vector<FPhysicsBodySnapshot>
	{
		std::vector<FPhysicsBodySnapshot> Result;
		if (!IsOwningThread()) return Result;
		Result.reserve(Bodies.size());
		for (const FBodyRecord& Body : Bodies) Result.push_back({GetHandle(Body), Body.Desc});
		return Result;
	}

	auto FPhysicsScene::SetQueryExecutionPolicy(EPhysicsSceneQueryExecutionPolicy Policy) -> bool
	{
		if (!IsOwningThread() || !IsValidPolicy(Policy)) return false;
		QueryExecutionPolicy = Policy;
		return true;
	}

	auto FPhysicsScene::SetDetailedQueryDiagnosticsEnabled(bool bEnabled) -> bool
	{
		if (!IsOwningThread()) return false;
		Diagnostics.bDetailedDiagnosticsEnabled = bEnabled;
		return true;
	}

	auto FPhysicsScene::CaptureQueryDiagnostics() const -> FPhysicsSceneQueryDiagnostics
	{
		return IsOwningThread() ? Diagnostics : FPhysicsSceneQueryDiagnostics{};
	}

	auto FPhysicsScene::ResetQueryDiagnostics() -> bool
	{
		if (!IsOwningThread()) return false;
		const bool bDetailedDiagnosticsEnabled = Diagnostics.bDetailedDiagnosticsEnabled;
		const uint64 StaticBodies = Diagnostics.Mutations.StaticBodies;
		const uint64 KinematicBodies = Diagnostics.Mutations.KinematicBodies;
		const uint64 DynamicBodies = Diagnostics.Mutations.DynamicBodies;
		const uint64 RetainedSpatialBytes = Diagnostics.Mutations.RetainedSpatialBytes;
		const uint64 UniqueGeometryResources = Diagnostics.Mutations.UniqueGeometryResources;
		const uint64 RetainedGeometryBytes = Diagnostics.Mutations.RetainedGeometryBytes;
		Diagnostics = {};
		Diagnostics.bDetailedDiagnosticsEnabled = bDetailedDiagnosticsEnabled;
		Diagnostics.Mutations.BodiesAtReset = static_cast<uint64>(Bodies.size());
		Diagnostics.Mutations.BodiesPresent = static_cast<uint64>(Bodies.size());
		Diagnostics.Mutations.StaticBodies = StaticBodies;
		Diagnostics.Mutations.KinematicBodies = KinematicBodies;
		Diagnostics.Mutations.DynamicBodies = DynamicBodies;
		Diagnostics.Mutations.RetainedSpatialBytes = RetainedSpatialBytes;
		Diagnostics.Mutations.UniqueGeometryResources = UniqueGeometryResources;
		Diagnostics.Mutations.RetainedGeometryBytes = RetainedGeometryBytes;
		return true;
	}

	auto FPhysicsScene::LineTraceSingle(
		const FVector3& Start,
		const FVector3& End,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit) const -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.LineTraceSingle");
		FPhysicsSceneLastQueryDiagnostics Query = BeginQuery(EPhysicsSceneQueryKind::LineTraceSingle);
		const uint64 TimingStart = BeginDetailedTiming();
		OutHit = {};
		if (!IsOwningThread())
		{
			Query.bOffThreadRejected = true;
			Query.Counters.OffThreadQueries = 1;
			CommitQuery(Query, TimingStart);
			return false;
		}
		if (!IsLineTraceValid(Start, End, Filter))
		{
			Query.Counters.InvalidQueries = 1;
			CommitQuery(Query, TimingStart);
			return false;
		}
		Query.bInputValid = true;
		bool bResult = false;
		switch (QueryExecutionPolicy)
		{
		case EPhysicsSceneQueryExecutionPolicy::Reference:
			Query.Counters.ReferenceExecutions = 1;
			bResult = ExecuteReferenceLineTrace(Start, End, Filter, OutHit, Query.Counters);
			break;
		case EPhysicsSceneQueryExecutionPolicy::Production:
			Query.Counters.ProductionExecutions = 1;
			bResult = ExecuteProductionLineTrace(Start, End, Filter, OutHit, Query.Counters);
			break;
		case EPhysicsSceneQueryExecutionPolicy::Compare:
		{
			Query.Counters.ReferenceExecutions = 1;
			Query.Counters.ProductionExecutions = 1;
			Query.Counters.CompareExecutions = 1;
			const FVector3 QueryStart = Start;
			const FVector3 QueryEnd = End;
			const FPhysicsQueryFilter QueryFilter = Filter;
			Query.Counters.CaptureHighWater = static_cast<uint64>(QueryFilter.IgnoredActors.size());
			Query.Counters.ScratchHighWater = 2;
			FPhysicsQueryHit ReferenceHit;
			FPhysicsQueryHit ProductionHit;
			const bool bReferenceStatus = ExecuteReferenceLineTrace(
				QueryStart, QueryEnd, QueryFilter, ReferenceHit, Query.Counters);
			const bool bProductionStatus = ExecuteProductionLineTrace(
				QueryStart, QueryEnd, QueryFilter, ProductionHit, Query.Counters);
			FPhysicsSceneQueryMismatch Mismatch;
			if (RecordSingleMismatch(
				EPhysicsSceneQueryKind::LineTraceSingle,
				bReferenceStatus,
				ReferenceHit,
				bProductionStatus,
				ProductionHit,
				Mismatch))
			{
				Query.Counters.CompareMismatches = 1;
				Query.Counters.Fallbacks = 1;
				if (Diagnostics.bDetailedDiagnosticsEnabled)
				{
					Diagnostics.LastMismatch = Mismatch;
					Query.Counters.CaptureHighWater = std::max<uint64>(Query.Counters.CaptureHighWater, 1);
				}
				OutHit = ReferenceHit;
				bResult = bReferenceStatus;
				break;
			}
			OutHit = ProductionHit;
			bResult = bProductionStatus;
			break;
		}
		}
		Query.bResult = bResult;
		Query.Counters.ReturnedResults = bResult ? 1u : 0u;
		CommitQuery(Query, TimingStart);
		return bResult;
	}

	auto FPhysicsScene::SweepSingle(
		const FCollisionShape& Shape,
		const FTransform& StartTransform,
		const FVector3& Delta,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit) const -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.SweepSingle");
		FPhysicsSceneLastQueryDiagnostics Query = BeginQuery(EPhysicsSceneQueryKind::SweepSingle);
		const uint64 TimingStart = BeginDetailedTiming();
		OutHit = {};
		if (!IsOwningThread())
		{
			Query.bOffThreadRejected = true;
			Query.Counters.OffThreadQueries = 1;
			CommitQuery(Query, TimingStart);
			return false;
		}
		if (!IsSweepValid(Shape, StartTransform, Delta, Filter))
		{
			Query.Counters.InvalidQueries = 1;
			CommitQuery(Query, TimingStart);
			return false;
		}
		Query.bInputValid = true;
		bool bResult = false;
		switch (QueryExecutionPolicy)
		{
		case EPhysicsSceneQueryExecutionPolicy::Reference:
			Query.Counters.ReferenceExecutions = 1;
			bResult = ExecuteReferenceSweep(Shape, StartTransform, Delta, Filter, OutHit, Query.Counters);
			break;
		case EPhysicsSceneQueryExecutionPolicy::Production:
			Query.Counters.ProductionExecutions = 1;
			bResult = ExecuteProductionSweep(Shape, StartTransform, Delta, Filter, OutHit, Query.Counters);
			break;
		case EPhysicsSceneQueryExecutionPolicy::Compare:
		{
			Query.Counters.ReferenceExecutions = 1;
			Query.Counters.ProductionExecutions = 1;
			Query.Counters.CompareExecutions = 1;
			const FCollisionShape QueryShape = Shape;
			const FTransform QueryTransform = StartTransform;
			const FVector3 QueryDelta = Delta;
			const FPhysicsQueryFilter QueryFilter = Filter;
			Query.Counters.CaptureHighWater = static_cast<uint64>(QueryFilter.IgnoredActors.size());
			Query.Counters.ScratchHighWater = 2;
			FPhysicsQueryHit ReferenceHit;
			FPhysicsQueryHit ProductionHit;
			const bool bReferenceStatus = ExecuteReferenceSweep(
				QueryShape, QueryTransform, QueryDelta, QueryFilter, ReferenceHit, Query.Counters);
			const bool bProductionStatus = ExecuteProductionSweep(
				QueryShape, QueryTransform, QueryDelta, QueryFilter, ProductionHit, Query.Counters);
			FPhysicsSceneQueryMismatch Mismatch;
			if (RecordSingleMismatch(
				EPhysicsSceneQueryKind::SweepSingle,
				bReferenceStatus,
				ReferenceHit,
				bProductionStatus,
				ProductionHit,
				Mismatch))
			{
				Query.Counters.CompareMismatches = 1;
				Query.Counters.Fallbacks = 1;
				if (Diagnostics.bDetailedDiagnosticsEnabled)
				{
					Diagnostics.LastMismatch = Mismatch;
					Query.Counters.CaptureHighWater = std::max<uint64>(Query.Counters.CaptureHighWater, 1);
				}
				OutHit = ReferenceHit;
				bResult = bReferenceStatus;
				break;
			}
			OutHit = ProductionHit;
			bResult = bProductionStatus;
			break;
		}
		}
		Query.bResult = bResult;
		Query.Counters.ReturnedResults = bResult ? 1u : 0u;
		CommitQuery(Query, TimingStart);
		return bResult;
	}

	auto FPhysicsScene::OverlapMulti(
		const FCollisionShape& Shape,
		const FTransform& Transform,
		const FPhysicsQueryFilter& Filter,
		std::vector<FPhysicsQueryHit>& OutHits) const -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.OverlapMulti");
		FPhysicsSceneLastQueryDiagnostics Query = BeginQuery(EPhysicsSceneQueryKind::OverlapMulti);
		const uint64 TimingStart = BeginDetailedTiming();
		OutHits.clear();
		if (!IsOwningThread())
		{
			Query.bOffThreadRejected = true;
			Query.Counters.OffThreadQueries = 1;
			CommitQuery(Query, TimingStart);
			return false;
		}
		if (!IsOverlapValid(Shape, Transform, Filter))
		{
			Query.Counters.InvalidQueries = 1;
			CommitQuery(Query, TimingStart);
			return false;
		}
		Query.bInputValid = true;
		bool bResult = false;
		switch (QueryExecutionPolicy)
		{
		case EPhysicsSceneQueryExecutionPolicy::Reference:
			Query.Counters.ReferenceExecutions = 1;
			bResult = ExecuteReferenceOverlap(Shape, Transform, Filter, OutHits, Query.Counters);
			break;
		case EPhysicsSceneQueryExecutionPolicy::Production:
			Query.Counters.ProductionExecutions = 1;
			bResult = ExecuteProductionOverlap(Shape, Transform, Filter, OutHits, Query.Counters);
			break;
		case EPhysicsSceneQueryExecutionPolicy::Compare:
		{
			Query.Counters.ReferenceExecutions = 1;
			Query.Counters.ProductionExecutions = 1;
			Query.Counters.CompareExecutions = 1;
			const FCollisionShape QueryShape = Shape;
			const FTransform QueryTransform = Transform;
			const FPhysicsQueryFilter QueryFilter = Filter;
			Query.Counters.CaptureHighWater = static_cast<uint64>(QueryFilter.IgnoredActors.size());
			std::vector<FPhysicsQueryHit> ReferenceHits;
			std::vector<FPhysicsQueryHit> ProductionHits;
			const bool bReferenceStatus = ExecuteReferenceOverlap(
				QueryShape, QueryTransform, QueryFilter, ReferenceHits, Query.Counters);
			const bool bProductionStatus = ExecuteProductionOverlap(
				QueryShape, QueryTransform, QueryFilter, ProductionHits, Query.Counters);
			FPhysicsSceneQueryMismatch Mismatch;
			if (RecordMultiMismatch(
				bReferenceStatus, ReferenceHits, bProductionStatus, ProductionHits, Mismatch))
			{
				Query.Counters.CompareMismatches = 1;
				Query.Counters.Fallbacks = 1;
				if (Diagnostics.bDetailedDiagnosticsEnabled)
				{
					Diagnostics.LastMismatch = Mismatch;
					Query.Counters.CaptureHighWater = std::max<uint64>(Query.Counters.CaptureHighWater, 1);
				}
				OutHits = std::move(ReferenceHits);
				bResult = bReferenceStatus;
				break;
			}
			OutHits = std::move(ProductionHits);
			bResult = bProductionStatus;
			break;
		}
		}
		Query.bResult = bResult;
		Query.Counters.ReturnedResults = static_cast<uint64>(OutHits.size());
		CommitQuery(Query, TimingStart);
		return bResult;
	}

	auto FPhysicsScene::IsValidPolicy(EPhysicsSceneQueryExecutionPolicy Policy) const -> bool
	{
		switch (Policy)
		{
		case EPhysicsSceneQueryExecutionPolicy::Reference:
		case EPhysicsSceneQueryExecutionPolicy::Production:
		case EPhysicsSceneQueryExecutionPolicy::Compare:
			return true;
		}
		return false;
	}

	auto FPhysicsScene::IsLineTraceValid(
		const FVector3& Start,
		const FVector3& End,
		const FPhysicsQueryFilter& Filter) const -> bool
	{
		return IsOwningThread() && Math::IsFinite(Start) && Math::IsFinite(End)
			&& Filter.QueryChannel < MaximumPhysicsChannels;
	}

	auto FPhysicsScene::IsSweepValid(
		const FCollisionShape& Shape,
		const FTransform& StartTransform,
		const FVector3& Delta,
		const FPhysicsQueryFilter& Filter) const -> bool
	{
		return IsOwningThread() && Shape.IsValid() && IsValidPhysicsTransform(StartTransform)
			&& Math::IsFinite(Delta) && Filter.QueryChannel < MaximumPhysicsChannels;
	}

	auto FPhysicsScene::IsOverlapValid(
		const FCollisionShape& Shape,
		const FTransform& Transform,
		const FPhysicsQueryFilter& Filter) const -> bool
	{
		return IsOwningThread() && Shape.IsValid() && IsValidPhysicsTransform(Transform)
			&& Filter.QueryChannel < MaximumPhysicsChannels;
	}

	auto FPhysicsScene::ExecuteReferenceLineTrace(
		const FVector3& Start,
		const FVector3& End,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit,
		FPhysicsSceneQueryCounters& Work) const -> bool
	{
		for (const FBodyRecord& Body : Bodies)
		{
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(GetHandle(Body), Filter))
			{
				SaturatingAdd(Work.IgnoredBodies, 1);
				continue;
			}
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response != EPhysicsQueryResponse::Block)
			{
				SaturatingAdd(Work.FilterRejectedBodies, 1);
				continue;
			}
			SaturatingAdd(Work.NarrowPhasePairTests, 1);
			FPhysicsQueryHit Candidate;
			CollisionGeometry::FCollisionGeometryCounters GeometryCounters;
			CollisionGeometry::ECollisionQueryStatus Status;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.Pair.Raycast");
				Status = CollisionGeometry::Raycast(Start, End, Body.Desc.Geometry, Body.Desc.Transform,
					CollisionGeometry::ECollisionQueryAlgorithm::Reference, Candidate, &GeometryCounters);
			}
			AccumulateGeometryCounters(Work, GeometryCounters);
			if (Status != CollisionGeometry::ECollisionQueryStatus::Hit) continue;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = GetHandle(Body);
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(End - Start) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		}
		return OutHit.IsHit();
	}

	auto FPhysicsScene::ExecuteProductionLineTrace(
		const FVector3& Start,
		const FVector3& End,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit,
		FPhysicsSceneQueryCounters& Work) const -> bool
	{
		std::array<float, 6> QueryBounds;
		if (!CompactBounds(Math::Min(Start, End), Math::Max(Start, End), QueryBounds)) return false;
		const bool bTraversalComplete = TraverseProductionCandidates(
			QueryBounds, Start, FVector3(0.0), End - Start, &OutHit, [&](const FBodyRecord& Body) {
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(GetHandle(Body), Filter))
			{
				SaturatingAdd(Work.IgnoredBodies, 1);
				return;
			}
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response != EPhysicsQueryResponse::Block)
			{
				SaturatingAdd(Work.FilterRejectedBodies, 1);
				return;
			}
			SaturatingAdd(Work.NarrowPhasePairTests, 1);
			FPhysicsQueryHit Candidate;
			CollisionGeometry::FCollisionGeometryCounters GeometryCounters;
			CollisionGeometry::ECollisionQueryStatus Status;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.Pair.Raycast");
				Status = CollisionGeometry::Raycast(Start, End, Body.Desc.Geometry, Body.Desc.Transform,
					CollisionGeometry::ECollisionQueryAlgorithm::Production, Candidate, &GeometryCounters);
			}
			AccumulateGeometryCounters(Work, GeometryCounters);
			if (Status != CollisionGeometry::ECollisionQueryStatus::Hit) return;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = GetHandle(Body);
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(End - Start) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		}, Work);
		if (!bTraversalComplete)
		{
			SaturatingAdd(Work.Fallbacks, 1);
			SaturatingAdd(Diagnostics.Mutations.SpatialFallbacks, 1);
			SaturatingAdd(Diagnostics.Mutations.ScratchOverflows, 1);
			OutHit = {};
			return ExecuteReferenceLineTrace(Start, End, Filter, OutHit, Work);
		}
		if (ProductionTestFault == EProductionTestFault::CorruptFirstResult && OutHit.IsHit()) ++OutHit.UserToken;
		return OutHit.IsHit();
	}

	auto FPhysicsScene::ExecuteReferenceSweep(
		const FCollisionShape& Shape,
		const FTransform& StartTransform,
		const FVector3& Delta,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit,
		FPhysicsSceneQueryCounters& Work) const -> bool
	{
		for (const FBodyRecord& Body : Bodies)
		{
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(GetHandle(Body), Filter))
			{
				SaturatingAdd(Work.IgnoredBodies, 1);
				continue;
			}
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response != EPhysicsQueryResponse::Block)
			{
				SaturatingAdd(Work.FilterRejectedBodies, 1);
				continue;
			}
			SaturatingAdd(Work.NarrowPhasePairTests, 1);
			FPhysicsQueryHit Candidate;
			CollisionGeometry::FCollisionGeometryCounters GeometryCounters;
			CollisionGeometry::ECollisionQueryStatus Status;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.Pair.Sweep");
				Status = CollisionGeometry::Sweep(
					Shape,
					StartTransform,
					Delta,
					Body.Desc.Geometry,
					Body.Desc.Transform,
					CollisionGeometry::ECollisionQueryAlgorithm::Reference,
					Candidate,
					&GeometryCounters);
			}
			AccumulateGeometryCounters(Work, GeometryCounters);
			if (Status != CollisionGeometry::ECollisionQueryStatus::Hit) continue;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = GetHandle(Body);
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(Delta) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		}
		return OutHit.IsHit();
	}

	auto FPhysicsScene::ExecuteProductionSweep(
		const FCollisionShape& Shape,
		const FTransform& StartTransform,
		const FVector3& Delta,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit,
		FPhysicsSceneQueryCounters& Work) const -> bool
	{
		std::array<float, 6> QueryBounds;
		if (!BuildQueryBounds(Shape, StartTransform, Delta, QueryBounds)) return false;
		FVector3 SweepMin;
		FVector3 SweepMax;
		if (!BuildShapeBounds(Shape, StartTransform, SweepMin, SweepMax)) return false;
		const bool bTraversalComplete = TraverseProductionCandidates(
			QueryBounds, (SweepMin + SweepMax) * 0.5, (SweepMax - SweepMin) * 0.5, Delta, &OutHit,
			[&](const FBodyRecord& Body) {
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(GetHandle(Body), Filter))
			{
				SaturatingAdd(Work.IgnoredBodies, 1);
				return;
			}
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response != EPhysicsQueryResponse::Block)
			{
				SaturatingAdd(Work.FilterRejectedBodies, 1);
				return;
			}
			SaturatingAdd(Work.NarrowPhasePairTests, 1);
			FPhysicsQueryHit Candidate;
			CollisionGeometry::FCollisionGeometryCounters GeometryCounters;
			CollisionGeometry::ECollisionQueryStatus Status;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.Pair.Sweep");
				Status = CollisionGeometry::Sweep(
					Shape,
					StartTransform,
					Delta,
					Body.Desc.Geometry,
					Body.Desc.Transform,
					CollisionGeometry::ECollisionQueryAlgorithm::Production,
					Candidate,
					&GeometryCounters);
			}
			AccumulateGeometryCounters(Work, GeometryCounters);
			if (Status != CollisionGeometry::ECollisionQueryStatus::Hit) return;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = GetHandle(Body);
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(Delta) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		}, Work);
		if (!bTraversalComplete)
		{
			SaturatingAdd(Work.Fallbacks, 1);
			SaturatingAdd(Diagnostics.Mutations.SpatialFallbacks, 1);
			SaturatingAdd(Diagnostics.Mutations.ScratchOverflows, 1);
			OutHit = {};
			return ExecuteReferenceSweep(Shape, StartTransform, Delta, Filter, OutHit, Work);
		}
		if (ProductionTestFault == EProductionTestFault::CorruptFirstResult && OutHit.IsHit()) ++OutHit.UserToken;
		return OutHit.IsHit();
	}

	auto FPhysicsScene::ExecuteReferenceOverlap(
		const FCollisionShape& Shape,
		const FTransform& Transform,
		const FPhysicsQueryFilter& Filter,
		std::vector<FPhysicsQueryHit>& OutHits,
		FPhysicsSceneQueryCounters& Work) const -> bool
	{
		for (const FBodyRecord& Body : Bodies)
		{
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(GetHandle(Body), Filter))
			{
				SaturatingAdd(Work.IgnoredBodies, 1);
				continue;
			}
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response == EPhysicsQueryResponse::Ignore)
			{
				SaturatingAdd(Work.FilterRejectedBodies, 1);
				continue;
			}
			SaturatingAdd(Work.NarrowPhasePairTests, 1);
			FPhysicsQueryHit Hit;
			CollisionGeometry::FCollisionGeometryCounters GeometryCounters;
			CollisionGeometry::ECollisionQueryStatus Status;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.Pair.Overlap");
				Status = CollisionGeometry::Overlap(Shape, Transform, Body.Desc.Geometry, Body.Desc.Transform,
					CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &GeometryCounters);
			}
			AccumulateGeometryCounters(Work, GeometryCounters);
			if (Status != CollisionGeometry::ECollisionQueryStatus::Hit) continue;
			SaturatingAdd(Work.RawHits, 1);
			Hit.ActorHandle = GetHandle(Body);
			Hit.Response = Response;
			Hit.UserToken = Body.Desc.UserToken;
			OutHits.push_back(Hit);
		}
		SortMultiResults(OutHits);
		return !OutHits.empty();
	}

	auto FPhysicsScene::ExecuteProductionOverlap(
		const FCollisionShape& Shape,
		const FTransform& Transform,
		const FPhysicsQueryFilter& Filter,
		std::vector<FPhysicsQueryHit>& OutHits,
		FPhysicsSceneQueryCounters& Work) const -> bool
	{
		std::array<float, 6> QueryBounds;
		if (!BuildQueryBounds(Shape, Transform, FVector3(0.0), QueryBounds)) return false;
		const bool bTraversalComplete = TraverseProductionCandidates(
			QueryBounds, FVector3(0.0), FVector3(0.0), FVector3(0.0), nullptr,
			[&](const FBodyRecord& Body) {
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(GetHandle(Body), Filter))
			{
				SaturatingAdd(Work.IgnoredBodies, 1);
				return;
			}
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response == EPhysicsQueryResponse::Ignore)
			{
				SaturatingAdd(Work.FilterRejectedBodies, 1);
				return;
			}
			SaturatingAdd(Work.NarrowPhasePairTests, 1);
			FPhysicsQueryHit Hit;
			CollisionGeometry::FCollisionGeometryCounters GeometryCounters;
			CollisionGeometry::ECollisionQueryStatus Status;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Physics.Query.Pair.Overlap");
				Status = CollisionGeometry::Overlap(Shape, Transform, Body.Desc.Geometry, Body.Desc.Transform,
					CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit, &GeometryCounters);
			}
			AccumulateGeometryCounters(Work, GeometryCounters);
			if (Status != CollisionGeometry::ECollisionQueryStatus::Hit) return;
			SaturatingAdd(Work.RawHits, 1);
			Hit.ActorHandle = GetHandle(Body);
			Hit.Response = Response;
			Hit.UserToken = Body.Desc.UserToken;
			OutHits.push_back(Hit);
		}, Work);
		if (!bTraversalComplete)
		{
			SaturatingAdd(Work.Fallbacks, 1);
			SaturatingAdd(Diagnostics.Mutations.SpatialFallbacks, 1);
			SaturatingAdd(Diagnostics.Mutations.ScratchOverflows, 1);
			OutHits.clear();
			return ExecuteReferenceOverlap(Shape, Transform, Filter, OutHits, Work);
		}
		SortMultiResults(OutHits);
		if (ProductionTestFault == EProductionTestFault::ReverseResults) std::ranges::reverse(OutHits);
		if (ProductionTestFault == EProductionTestFault::CorruptFirstResult && !OutHits.empty())
			++OutHits.front().UserToken;
		return !OutHits.empty();
	}

	auto FPhysicsScene::RecordSingleMismatch(
		EPhysicsSceneQueryKind QueryKind,
		bool bReferenceStatus,
		const FPhysicsQueryHit& ReferenceHit,
		bool bProductionStatus,
		const FPhysicsQueryHit& ProductionHit,
		FPhysicsSceneQueryMismatch& OutMismatch) const -> bool
	{
		uint32 Difference = BuildHitDifference(ReferenceHit, ProductionHit);
		if (bReferenceStatus != bProductionStatus) Difference |= HitDifferenceStatus;
		if (Difference == 0) return false;
		OutMismatch = {
			.bValid = true,
			.QueryKind = QueryKind,
			.bReferenceStatus = bReferenceStatus,
			.bProductionStatus = bProductionStatus,
			.ReferenceCount = bReferenceStatus ? 1u : 0u,
			.ProductionCount = bProductionStatus ? 1u : 0u,
			.FirstDifferingResult = 0,
			.ReferenceWinner = ReferenceHit.ActorHandle,
			.ProductionWinner = ProductionHit.ActorHandle,
			.DifferenceMask = Difference};
		return true;
	}

	auto FPhysicsScene::RecordMultiMismatch(
		bool bReferenceStatus,
		const std::vector<FPhysicsQueryHit>& ReferenceHits,
		bool bProductionStatus,
		const std::vector<FPhysicsQueryHit>& ProductionHits,
		FPhysicsSceneQueryMismatch& OutMismatch) const -> bool
	{
		uint32 Difference = 0;
		if (bReferenceStatus != bProductionStatus) Difference |= HitDifferenceStatus;
		if (ReferenceHits.size() != ProductionHits.size()) Difference |= HitDifferenceCount;
		const size_t CommonCount = std::min(ReferenceHits.size(), ProductionHits.size());
		size_t FirstDifference = CommonCount;
		for (size_t Index = 0; Index < CommonCount; ++Index)
		{
			const uint32 HitDifference = BuildHitDifference(ReferenceHits[Index], ProductionHits[Index]);
			if (HitDifference == 0) continue;
			if (FirstDifference == CommonCount) FirstDifference = Index;
			Difference |= HitDifference;
			if (ReferenceHits[Index].ActorHandle != ProductionHits[Index].ActorHandle)
				Difference |= HitDifferenceOrder;
		}
		if (Difference == 0) return false;
		OutMismatch = {
			.bValid = true,
			.QueryKind = EPhysicsSceneQueryKind::OverlapMulti,
			.bReferenceStatus = bReferenceStatus,
			.bProductionStatus = bProductionStatus,
			.ReferenceCount = BoundedCount(ReferenceHits.size()),
			.ProductionCount = BoundedCount(ProductionHits.size()),
			.FirstDifferingResult = BoundedCount(FirstDifference),
			.ReferenceWinner = ReferenceHits.empty() ? FPhysicsActorHandle{} : ReferenceHits.front().ActorHandle,
			.ProductionWinner = ProductionHits.empty() ? FPhysicsActorHandle{} : ProductionHits.front().ActorHandle,
			.DifferenceMask = Difference};
		return true;
	}

	auto FPhysicsScene::BeginQuery(EPhysicsSceneQueryKind QueryKind) const -> FPhysicsSceneLastQueryDiagnostics
	{
		FPhysicsSceneLastQueryDiagnostics Query;
		Query.bValid = true;
		Query.QueryKind = QueryKind;
		Query.Policy = QueryExecutionPolicy;
		Query.Counters.SubmittedQueries = 1;
		return Query;
	}

	auto FPhysicsScene::BeginDetailedTiming() const -> uint64
	{
		if (!Diagnostics.bDetailedDiagnosticsEnabled) return 0;
		const int64 Nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		return Nanoseconds > 0 ? static_cast<uint64>(Nanoseconds) : 1;
	}

	auto FPhysicsScene::CommitQuery(
		FPhysicsSceneLastQueryDiagnostics& Query,
		uint64 StartNanoseconds) const -> void
	{
		if (StartNanoseconds != 0)
		{
			const int64 CurrentNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			const uint64 EndNanoseconds = CurrentNanoseconds > 0
				? static_cast<uint64>(CurrentNanoseconds)
				: StartNanoseconds;
			Query.Counters.DetailedTimingSamples = 1;
			Query.Counters.DetailedTimingNanoseconds = EndNanoseconds >= StartNanoseconds
				? EndNanoseconds - StartNanoseconds
				: 0;
		}
		const size_t QueryIndex = static_cast<size_t>(Query.QueryKind);
		if (QueryIndex < Diagnostics.Queries.size())
			MergeQueryCounters(Diagnostics.Queries[QueryIndex], Query.Counters);
		Diagnostics.LastQuery = Query;
	}

	auto FPhysicsScene::MergeQueryCounters(
		FPhysicsSceneQueryCounters& Target,
		const FPhysicsSceneQueryCounters& Source) const -> void
	{
		SaturatingAdd(Target.SubmittedQueries, Source.SubmittedQueries);
		SaturatingAdd(Target.InvalidQueries, Source.InvalidQueries);
		SaturatingAdd(Target.OffThreadQueries, Source.OffThreadQueries);
		SaturatingAdd(Target.ReferenceExecutions, Source.ReferenceExecutions);
		SaturatingAdd(Target.ProductionExecutions, Source.ProductionExecutions);
		SaturatingAdd(Target.CompareExecutions, Source.CompareExecutions);
		SaturatingAdd(Target.BodyVisits, Source.BodyVisits);
		SaturatingAdd(Target.Candidates, Source.Candidates);
		SaturatingAdd(Target.IgnoredBodies, Source.IgnoredBodies);
		SaturatingAdd(Target.FilterRejectedBodies, Source.FilterRejectedBodies);
		SaturatingAdd(Target.NarrowPhasePairTests, Source.NarrowPhasePairTests);
		SaturatingAdd(Target.GeometryDistanceEvaluations, Source.GeometryDistanceEvaluations);
		SaturatingAdd(Target.GeometrySearchIterations, Source.GeometrySearchIterations);
		SaturatingAdd(Target.NarrowPhaseLeafTests, Source.NarrowPhaseLeafTests);
		SaturatingAdd(Target.GeometryFeatureTests, Source.GeometryFeatureTests);
		SaturatingAdd(Target.AssetNodeTests, Source.AssetNodeTests);
		SaturatingAdd(Target.AssetLeafTests, Source.AssetLeafTests);
		SaturatingAdd(Target.HeightFieldCellTests, Source.HeightFieldCellTests);
		SaturatingAdd(Target.HeightFieldTriangleTests, Source.HeightFieldTriangleTests);
		SaturatingAdd(Target.CompoundChildrenTested, Source.CompoundChildrenTested);
		SaturatingAdd(Target.AnalyticDispatches, Source.AnalyticDispatches);
		SaturatingAdd(Target.GenericDispatches, Source.GenericDispatches);
		SaturatingAdd(Target.GeometrySupportEvaluations, Source.GeometrySupportEvaluations);
		SaturatingAdd(Target.UnsupportedPairs, Source.UnsupportedPairs);
		SaturatingAdd(Target.NonConvergedPairs, Source.NonConvergedPairs);
		SaturatingAdd(Target.ReferencePairFallbacks, Source.ReferencePairFallbacks);
		SaturatingAdd(Target.RawHits, Source.RawHits);
		SaturatingAdd(Target.ReturnedResults, Source.ReturnedResults);
		SaturatingAdd(Target.Fallbacks, Source.Fallbacks);
		SaturatingAdd(Target.CompareMismatches, Source.CompareMismatches);
		Target.ScratchHighWater = std::max(Target.ScratchHighWater, Source.ScratchHighWater);
		Target.CaptureHighWater = std::max(Target.CaptureHighWater, Source.CaptureHighWater);
		SaturatingAdd(Target.NodeTests, Source.NodeTests);
		SaturatingAdd(Target.BoundTests, Source.BoundTests);
		SaturatingAdd(Target.PrunedNodes, Source.PrunedNodes);
		SaturatingAdd(Target.DetailedTimingSamples, Source.DetailedTimingSamples);
		SaturatingAdd(Target.DetailedTimingNanoseconds, Source.DetailedTimingNanoseconds);
	}

	auto FPhysicsScene::AccumulateGeometryCounters(
		FPhysicsSceneQueryCounters& Target,
		const CollisionGeometry::FCollisionGeometryCounters& Source) const -> void
	{
		SaturatingAdd(Target.GeometryDistanceEvaluations, Source.DistanceEvaluations);
		SaturatingAdd(Target.GeometrySearchIterations, Source.SearchIterations);
		SaturatingAdd(Target.NarrowPhaseLeafTests, Source.LeafTests);
		SaturatingAdd(Target.GeometryFeatureTests, Source.FeatureTests);
		SaturatingAdd(Target.AssetNodeTests, Source.AssetNodeTests);
		SaturatingAdd(Target.AssetLeafTests, Source.AssetLeafTests);
		SaturatingAdd(Target.HeightFieldCellTests, Source.HeightFieldCellTests);
		SaturatingAdd(Target.HeightFieldTriangleTests, Source.HeightFieldTriangleTests);
		SaturatingAdd(Target.CompoundChildrenTested, Source.CompoundChildren);
		SaturatingAdd(Target.AnalyticDispatches, Source.AnalyticDispatches);
		SaturatingAdd(Target.GenericDispatches, Source.GenericDispatches);
		SaturatingAdd(Target.GeometrySupportEvaluations, Source.SupportEvaluations);
		SaturatingAdd(Target.UnsupportedPairs, Source.Unsupported);
		SaturatingAdd(Target.NonConvergedPairs, Source.NonConverged);
		SaturatingAdd(Target.ReferencePairFallbacks, Source.ReferenceFallbacks);
		if (Source.bOverflowed) Diagnostics.bOverflowed = true;
	}

	auto FPhysicsScene::SaturatingAdd(uint64& Target, uint64 Delta) const -> void
	{
		if (Target > std::numeric_limits<uint64>::max() - Delta)
		{
			Target = std::numeric_limits<uint64>::max();
			Diagnostics.bOverflowed = true;
			return;
		}
		Target += Delta;
	}

	auto FPhysicsScene::CountMutation(uint64& Counter) -> void
	{
		SaturatingAdd(Counter, 1);
	}

	auto FPhysicsScene::IsOwningThread() const -> bool
	{
		return std::this_thread::get_id() == OwningThread;
	}

	auto FPhysicsScene::FindBody(FPhysicsActorHandle Handle) -> FBodyRecord*
	{
		if (!Handle.IsValid() || Handle.Id > Slots.size()) return nullptr;
		FSlot& Slot = Slots[static_cast<size_t>(Handle.Id - 1)];
		if (Slot.Generation != Handle.Generation || Slot.DenseOrNext >= Bodies.size()) return nullptr;
		FBodyRecord& Body = Bodies[Slot.DenseOrNext];
		return Body.Slot == Handle.Id - 1 ? &Body : nullptr;
	}

	auto FPhysicsScene::FindBody(FPhysicsActorHandle Handle) const -> const FBodyRecord*
	{
		if (!Handle.IsValid() || Handle.Id > Slots.size()) return nullptr;
		const FSlot& Slot = Slots[static_cast<size_t>(Handle.Id - 1)];
		if (Slot.Generation != Handle.Generation || Slot.DenseOrNext >= Bodies.size()) return nullptr;
		const FBodyRecord& Body = Bodies[Slot.DenseOrNext];
		return Body.Slot == Handle.Id - 1 ? &Body : nullptr;
	}

	auto FPhysicsScene::GetBodyBySlot(uint32 SlotIndex) -> FBodyRecord*
	{
		if (SlotIndex >= Slots.size()) return nullptr;
		FSlot& Slot = Slots[SlotIndex];
		if (Slot.DenseOrNext >= Bodies.size()) return nullptr;
		FBodyRecord& Body = Bodies[Slot.DenseOrNext];
		return Body.Slot == SlotIndex ? &Body : nullptr;
	}

	auto FPhysicsScene::GetBodyBySlot(uint32 SlotIndex) const -> const FBodyRecord*
	{
		if (SlotIndex >= Slots.size()) return nullptr;
		const FSlot& Slot = Slots[SlotIndex];
		if (Slot.DenseOrNext >= Bodies.size()) return nullptr;
		const FBodyRecord& Body = Bodies[Slot.DenseOrNext];
		return Body.Slot == SlotIndex ? &Body : nullptr;
	}

	auto FPhysicsScene::GetHandle(const FBodyRecord& Body) const -> FPhysicsActorHandle
	{
		return {static_cast<uint64>(Body.Slot) + 1, Slots[Body.Slot].Generation};
	}

	auto FPhysicsScene::IsValidMotionType(EPhysicsBodyMotionType MotionType) const -> bool
	{
		return MotionType == EPhysicsBodyMotionType::Static
			|| MotionType == EPhysicsBodyMotionType::Kinematic
			|| MotionType == EPhysicsBodyMotionType::Dynamic;
	}

	auto FPhysicsScene::BuildBodyBounds(
		const FPhysicsBodyDesc& Desc, std::array<float, 6>& OutBounds) const -> bool
	{
		if (!Desc.Geometry.IsValid()) return false;
		if (Desc.Geometry.GetKind() == ECollisionGeometryKind::ConvexHull
			|| Desc.Geometry.GetKind() == ECollisionGeometryKind::TriangleMesh
			|| Desc.Geometry.GetKind() == ECollisionGeometryKind::HeightField)
		{
			FVector3 LocalMinimum;
			FVector3 LocalMaximum;
			if (!Desc.Geometry.GetLocalBounds(LocalMinimum, LocalMaximum)) return false;
			const FMatrix Matrix = Desc.Transform.ToMatrix();
			FVector3 Minimum(std::numeric_limits<double>::infinity());
			FVector3 Maximum(-std::numeric_limits<double>::infinity());
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Local(
					(Corner & 1u) ? LocalMaximum.x : LocalMinimum.x,
					(Corner & 2u) ? LocalMaximum.y : LocalMinimum.y,
					(Corner & 4u) ? LocalMaximum.z : LocalMinimum.z);
				const FVector3 World(Matrix * FVector4(Local, 1.0));
				Minimum = Math::Min(Minimum, World);
				Maximum = Math::Max(Maximum, World);
			}
			return CompactBounds(Minimum, Maximum, OutBounds);
		}
		FVector3 Min;
		FVector3 Max;
		bool bHasBounds = false;
		for (uint32 Index = 0; Index < Desc.Geometry.GetChildCount(); ++Index)
		{
			const FCollisionGeometryChild* Child = Desc.Geometry.GetChild(Index);
			if (!Child) return false;
			FVector3 ChildMin;
			FVector3 ChildMax;
			if (!BuildShapeBounds(Child->Shape, FTransform::Combine(Desc.Transform, Child->LocalTransform),
				ChildMin, ChildMax)) return false;
			if (!bHasBounds)
			{
				Min = ChildMin;
				Max = ChildMax;
				bHasBounds = true;
			}
			else
			{
				Min = Math::Min(Min, ChildMin);
				Max = Math::Max(Max, ChildMax);
			}
		}
		return bHasBounds && CompactBounds(Min, Max, OutBounds);
	}

	auto FPhysicsScene::BuildQueryBounds(const FCollisionShape& Shape, const FTransform& Transform,
		const FVector3& Delta, std::array<float, 6>& OutBounds) const -> bool
	{
		if (!Math::IsFinite(Delta)) return false;
		FVector3 StartMin;
		FVector3 StartMax;
		if (!BuildShapeBounds(Shape, Transform, StartMin, StartMax)) return false;
		return CompactBounds(Math::Min(StartMin, StartMin + Delta),
			Math::Max(StartMax, StartMax + Delta), OutBounds);
	}

	auto FPhysicsScene::InsertMovingProxy(uint32 SlotIndex) -> bool
	{
		FBodyRecord* Body = GetBodyBySlot(SlotIndex);
		if (!Body) return false;
		const uint32 Leaf = SpatialLeafBit | SlotIndex;
		Slots[SlotIndex].ProxyParent = InvalidSpatialIndex;
		if (MovingRoot == InvalidSpatialIndex)
		{
			MovingRoot = Leaf;
			CountMutation(Diagnostics.Mutations.MovingInsertions);
			return true;
		}
		auto GetBounds = [&](uint32 Reference) -> std::array<float, 6>
		{
			if ((Reference & SpatialLeafBit) != 0)
				return MovingProxyBounds(GetBodyBySlot(Reference & ~SpatialLeafBit)->Bounds);
			return MovingNodes[Reference].Bounds;
		};
		uint32 Sibling = MovingRoot;
		uint32 Depth = 0;
		while ((Sibling & SpatialLeafBit) == 0)
		{
			const FSpatialNode& Node = MovingNodes[Sibling];
			const double LeftCost = BoundsSurfaceArea(UnionBounds(GetBounds(Node.Left), MovingProxyBounds(Body->Bounds)))
				- BoundsSurfaceArea(GetBounds(Node.Left));
			const double RightCost = BoundsSurfaceArea(UnionBounds(GetBounds(Node.Right), MovingProxyBounds(Body->Bounds)))
				- BoundsSurfaceArea(GetBounds(Node.Right));
			Sibling = LeftCost < RightCost || (LeftCost == RightCost && Node.Left < Node.Right)
				? Node.Left : Node.Right;
			++Depth;
		}
		auto GetParent = [&](uint32 Reference) -> uint32
		{
			return (Reference & SpatialLeafBit) != 0
				? Slots[Reference & ~SpatialLeafBit].ProxyParent : MovingNodes[Reference].Parent;
		};
		auto SetParent = [&](uint32 Reference, uint32 Parent)
		{
			if ((Reference & SpatialLeafBit) != 0) Slots[Reference & ~SpatialLeafBit].ProxyParent = Parent;
			else MovingNodes[Reference].Parent = Parent;
		};
		const uint32 OldParent = GetParent(Sibling);
		uint32 NewParent;
		if (FreeMovingNode != InvalidSpatialIndex)
		{
			NewParent = FreeMovingNode;
			FreeMovingNode = MovingNodes[NewParent].Parent;
			MovingNodes[NewParent] = {};
		}
		else
		{
			NewParent = static_cast<uint32>(MovingNodes.size());
			if (MovingNodes.size() == MovingNodes.capacity()) MovingNodes.reserve(MovingNodes.size() + 256);
			MovingNodes.push_back({});
		}
		FSpatialNode& ParentNode = MovingNodes[NewParent];
		ParentNode.Bounds = UnionBounds(GetBounds(Sibling), MovingProxyBounds(Body->Bounds));
		ParentNode.Parent = OldParent;
		ParentNode.Left = Sibling;
		ParentNode.Right = Leaf;
		SetParent(Sibling, NewParent);
		SetParent(Leaf, NewParent);
		if (OldParent == InvalidSpatialIndex) MovingRoot = NewParent;
		else
		{
			FSpatialNode& Node = MovingNodes[OldParent];
			(Node.Left == Sibling ? Node.Left : Node.Right) = NewParent;
		}
		uint32 Current = NewParent;
		while (Current != InvalidSpatialIndex)
		{
			FSpatialNode& Node = MovingNodes[Current];
			Node.Bounds = UnionBounds(GetBounds(Node.Left), GetBounds(Node.Right));
			CountMutation(Diagnostics.Mutations.MovingRefits);
			Current = Node.Parent;
		}
		CountMutation(Diagnostics.Mutations.MovingInsertions);
		return Depth < 64 || RebuildMovingTree();
	}

	auto FPhysicsScene::RemoveMovingProxy(uint32 SlotIndex) -> void
	{
		if (SlotIndex >= Slots.size()) return;
		const uint32 Leaf = SpatialLeafBit | SlotIndex;
		const uint32 Parent = Slots[SlotIndex].ProxyParent;
		if (MovingRoot == Leaf)
		{
			MovingRoot = InvalidSpatialIndex;
			Slots[SlotIndex].ProxyParent = InvalidSpatialIndex;
			CountMutation(Diagnostics.Mutations.MovingRemovals);
			return;
		}
		if (Parent == InvalidSpatialIndex || Parent >= MovingNodes.size()) return;
		auto GetBounds = [&](uint32 Reference) -> std::array<float, 6>
		{
			if ((Reference & SpatialLeafBit) != 0)
				return MovingProxyBounds(GetBodyBySlot(Reference & ~SpatialLeafBit)->Bounds);
			return MovingNodes[Reference].Bounds;
		};
		auto SetParent = [&](uint32 Reference, uint32 NewParent)
		{
			if ((Reference & SpatialLeafBit) != 0) Slots[Reference & ~SpatialLeafBit].ProxyParent = NewParent;
			else MovingNodes[Reference].Parent = NewParent;
		};
		FSpatialNode& ParentNode = MovingNodes[Parent];
		const uint32 Sibling = ParentNode.Left == Leaf ? ParentNode.Right : ParentNode.Left;
		const uint32 GrandParent = ParentNode.Parent;
		if (GrandParent == InvalidSpatialIndex) MovingRoot = Sibling;
		else
		{
			FSpatialNode& GrandNode = MovingNodes[GrandParent];
			(GrandNode.Left == Parent ? GrandNode.Left : GrandNode.Right) = Sibling;
		}
		SetParent(Sibling, GrandParent);
		Slots[SlotIndex].ProxyParent = InvalidSpatialIndex;
		MovingNodes[Parent].Parent = FreeMovingNode;
		MovingNodes[Parent].Left = InvalidSpatialIndex;
		MovingNodes[Parent].Right = InvalidSpatialIndex;
		FreeMovingNode = Parent;
		uint32 Current = GrandParent;
		while (Current != InvalidSpatialIndex)
		{
			FSpatialNode& Node = MovingNodes[Current];
			Node.Bounds = UnionBounds(GetBounds(Node.Left), GetBounds(Node.Right));
			CountMutation(Diagnostics.Mutations.MovingRefits);
			Current = Node.Parent;
		}
		CountMutation(Diagnostics.Mutations.MovingRemovals);
	}

	auto FPhysicsScene::RebuildMovingTree() -> bool
	{
		std::vector<uint32> Leaves;
		Leaves.reserve(Bodies.size());
		for (const FBodyRecord& Body : Bodies)
			if (Body.Desc.MotionType != EPhysicsBodyMotionType::Static) Leaves.push_back(Body.Slot);
		MovingNodes.clear();
		FreeMovingNode = InvalidSpatialIndex;
		MovingRoot = InvalidSpatialIndex;
		if (Leaves.empty()) return true;
		MovingNodes.reserve(Leaves.size() - 1);
		std::function<uint32(size_t, size_t, uint32)> Build = [&](size_t Begin, size_t End, uint32 Parent) -> uint32
		{
			if (End - Begin == 1)
			{
				Slots[Leaves[Begin]].ProxyParent = Parent;
				return SpatialLeafBit | Leaves[Begin];
			}
			std::array<float, 6> Bounds = MovingProxyBounds(GetBodyBySlot(Leaves[Begin])->Bounds);
			for (size_t Index = Begin + 1; Index < End; ++Index)
				Bounds = UnionBounds(Bounds, MovingProxyBounds(GetBodyBySlot(Leaves[Index])->Bounds));
			const float X = Bounds[3] - Bounds[0];
			const float Y = Bounds[4] - Bounds[1];
			const float Z = Bounds[5] - Bounds[2];
			const uint32 Axis = Z > std::max(X, Y) ? 2u : (Y > X ? 1u : 0u);
			std::stable_sort(Leaves.begin() + Begin, Leaves.begin() + End, [&](uint32 A, uint32 B)
			{
				const auto& BA = GetBodyBySlot(A)->Bounds;
				const auto& BB = GetBodyBySlot(B)->Bounds;
				const float CA = BA[Axis] + BA[Axis + 3];
				const float CB = BB[Axis] + BB[Axis + 3];
				return CA < CB || (CA == CB && A < B);
			});
			const uint32 NodeIndex = static_cast<uint32>(MovingNodes.size());
			MovingNodes.push_back({});
			const size_t Middle = Begin + (End - Begin) / 2;
			const uint32 Left = Build(Begin, Middle, NodeIndex);
			const uint32 Right = Build(Middle, End, NodeIndex);
			MovingNodes[NodeIndex] = {Bounds, Parent, Left, Right};
			return NodeIndex;
		};
		MovingRoot = Build(0, Leaves.size(), InvalidSpatialIndex);
		CountMutation(Diagnostics.Mutations.MovingTreeRebuilds);
		return true;
	}

	auto FPhysicsScene::RebuildStaticTree() const -> bool
	{
		if (!bStaticTreeDirty) return true;
		std::vector<uint32> Leaves;
		Leaves.reserve(Bodies.size());
		for (const FBodyRecord& Body : Bodies)
			if (Body.Desc.MotionType == EPhysicsBodyMotionType::Static) Leaves.push_back(Body.Slot);
		StaticNodes.clear();
		StaticRoot = InvalidSpatialIndex;
		if (!Leaves.empty())
		{
			StaticNodes.reserve(Leaves.size() - 1);
			std::function<uint32(size_t, size_t)> Build = [&](size_t Begin, size_t End) -> uint32
			{
				if (End - Begin == 1) return SpatialLeafBit | Leaves[Begin];
				std::array<float, 6> Bounds = GetBodyBySlot(Leaves[Begin])->Bounds;
				for (size_t Index = Begin + 1; Index < End; ++Index)
					Bounds = UnionBounds(Bounds, GetBodyBySlot(Leaves[Index])->Bounds);
				const float X = Bounds[3] - Bounds[0];
				const float Y = Bounds[4] - Bounds[1];
				const float Z = Bounds[5] - Bounds[2];
				const uint32 Axis = Z > std::max(X, Y) ? 2u : (Y > X ? 1u : 0u);
				std::stable_sort(Leaves.begin() + Begin, Leaves.begin() + End, [&](uint32 A, uint32 B)
				{
					const auto& BA = GetBodyBySlot(A)->Bounds;
					const auto& BB = GetBodyBySlot(B)->Bounds;
					const float CA = BA[Axis] + BA[Axis + 3];
					const float CB = BB[Axis] + BB[Axis + 3];
					return CA < CB || (CA == CB && A < B);
				});
				const uint32 NodeIndex = static_cast<uint32>(StaticNodes.size());
				StaticNodes.push_back({});
				const size_t Middle = Begin + (End - Begin) / 2;
				const uint32 Left = Build(Begin, Middle);
				const uint32 Right = Build(Middle, End);
				StaticNodes[NodeIndex] = {Bounds, InvalidSpatialIndex, Left, Right};
				return NodeIndex;
			};
			StaticRoot = Build(0, Leaves.size());
		}
		bStaticTreeDirty = false;
		SaturatingAdd(Diagnostics.Mutations.StaticBuilds, 1);
		Diagnostics.Mutations.RetainedSpatialBytes = Slots.capacity() * sizeof(FSlot)
			+ StaticNodes.capacity() * sizeof(FSpatialNode) + MovingNodes.capacity() * sizeof(FSpatialNode)
			+ Bodies.capacity() * (sizeof(FBodyRecord) > 176 ? sizeof(FBodyRecord) - 176 : 0);
		return true;
	}

	auto FPhysicsScene::RefreshSpatialDiagnostics() -> void
	{
		Diagnostics.Mutations.RetainedSpatialBytes = Slots.capacity() * sizeof(FSlot)
			+ StaticNodes.capacity() * sizeof(FSpatialNode) + MovingNodes.capacity() * sizeof(FSpatialNode)
			+ Bodies.capacity() * (sizeof(FBodyRecord) > 176 ? sizeof(FBodyRecord) - 176 : 0);
	}
}
