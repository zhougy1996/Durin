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

	}

	FPhysicsScene::FPhysicsScene()
		: OwningThread(std::this_thread::get_id())
	{
	}

	FPhysicsScene::~FPhysicsScene() = default;

	auto FPhysicsScene::AddBody(const FPhysicsBodyDesc& Desc) -> FPhysicsActorHandle
	{
		CountMutation(Diagnostics.Mutations.AddCalls);
		if (!IsOwningThread() || !Desc.Shape.IsValid() || !IsValidPhysicsTransform(Desc.Transform)
			|| Desc.Filter.ObjectChannel >= MaximumPhysicsChannels || NextHandleId == 0)
		{
			CountMutation(Diagnostics.Mutations.AddRejected);
			return {};
		}
		const FPhysicsActorHandle Handle{NextHandleId++, 1};
		Bodies.push_back({Handle, Desc});
		CountMutation(Diagnostics.Mutations.AddSuccesses);
		Diagnostics.Mutations.BodiesPresent = static_cast<uint64>(Bodies.size());
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
		const auto It = std::ranges::find(Bodies, Handle, &FBodyRecord::Handle);
		if (It == Bodies.end())
		{
			CountMutation(Diagnostics.Mutations.RemoveRejected);
			CountMutation(Diagnostics.Mutations.FailedLookups);
			return false;
		}
		Bodies.erase(It);
		CountMutation(Diagnostics.Mutations.RemoveSuccesses);
		Diagnostics.Mutations.BodiesPresent = static_cast<uint64>(Bodies.size());
		return true;
	}

	auto FPhysicsScene::UpdateBody(FPhysicsActorHandle Handle, const FPhysicsBodyDesc& Desc) -> bool
	{
		CountMutation(Diagnostics.Mutations.UpdateCalls);
		if (!IsOwningThread() || !Desc.Shape.IsValid() || !IsValidPhysicsTransform(Desc.Transform)
			|| Desc.Filter.ObjectChannel >= MaximumPhysicsChannels)
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
		Body->Desc = Desc;
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

	auto FPhysicsScene::CaptureBodies() const -> std::vector<FPhysicsBodySnapshot>
	{
		std::vector<FPhysicsBodySnapshot> Result;
		if (!IsOwningThread()) return Result;
		Result.reserve(Bodies.size());
		for (const FBodyRecord& Body : Bodies) Result.push_back({Body.Handle, Body.Desc});
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
		Diagnostics = {};
		Diagnostics.bDetailedDiagnosticsEnabled = bDetailedDiagnosticsEnabled;
		Diagnostics.Mutations.BodiesAtReset = static_cast<uint64>(Bodies.size());
		Diagnostics.Mutations.BodiesPresent = static_cast<uint64>(Bodies.size());
		return true;
	}

	auto FPhysicsScene::LineTraceSingle(
		const FVector3& Start,
		const FVector3& End,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit) const -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.LineTraceSingle");
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
		DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.SweepSingle");
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
		DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.OverlapMulti");
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
			Query.Counters.ScratchHighWater = static_cast<uint64>(ReferenceHits.size() + ProductionHits.size());
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
		if (QueryExecutionPolicy != EPhysicsSceneQueryExecutionPolicy::Compare)
			Query.Counters.ScratchHighWater = static_cast<uint64>(OutHits.size());
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
			if (IsIgnored(Body.Handle, Filter))
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
			bool bHit = false;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.Pair.RaycastBox");
				bHit = CollisionGeometry::RaycastBox(
					Start, End, Body.Desc.Shape, Body.Desc.Transform, Candidate, &GeometryCounters);
			}
			SaturatingAdd(Work.GeometryDistanceEvaluations, GeometryCounters.DistanceEvaluations);
			SaturatingAdd(Work.GeometrySearchIterations, GeometryCounters.SearchIterations);
			if (GeometryCounters.bOverflowed) Diagnostics.bOverflowed = true;
			if (!bHit) continue;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = Body.Handle;
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
		ForEachProductionCandidate([&](const FBodyRecord& Body) {
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(Body.Handle, Filter))
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
			bool bHit = false;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.Pair.RaycastBox");
				bHit = CollisionGeometry::RaycastBox(
					Start, End, Body.Desc.Shape, Body.Desc.Transform, Candidate, &GeometryCounters);
			}
			SaturatingAdd(Work.GeometryDistanceEvaluations, GeometryCounters.DistanceEvaluations);
			SaturatingAdd(Work.GeometrySearchIterations, GeometryCounters.SearchIterations);
			if (GeometryCounters.bOverflowed) Diagnostics.bOverflowed = true;
			if (!bHit) return;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = Body.Handle;
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(End - Start) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		});
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
			if (IsIgnored(Body.Handle, Filter))
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
			bool bHit = false;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.Pair.SweepCapsuleBox");
				bHit = CollisionGeometry::SweepCapsuleBox(
					Shape,
					StartTransform,
					Delta,
					Body.Desc.Shape,
					Body.Desc.Transform,
					Candidate,
					&GeometryCounters);
			}
			SaturatingAdd(Work.GeometryDistanceEvaluations, GeometryCounters.DistanceEvaluations);
			SaturatingAdd(Work.GeometrySearchIterations, GeometryCounters.SearchIterations);
			if (GeometryCounters.bOverflowed) Diagnostics.bOverflowed = true;
			if (!bHit) continue;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = Body.Handle;
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
		ForEachProductionCandidate([&](const FBodyRecord& Body) {
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(Body.Handle, Filter))
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
			bool bHit = false;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.Pair.SweepCapsuleBox");
				bHit = CollisionGeometry::SweepCapsuleBox(
					Shape,
					StartTransform,
					Delta,
					Body.Desc.Shape,
					Body.Desc.Transform,
					Candidate,
					&GeometryCounters);
			}
			SaturatingAdd(Work.GeometryDistanceEvaluations, GeometryCounters.DistanceEvaluations);
			SaturatingAdd(Work.GeometrySearchIterations, GeometryCounters.SearchIterations);
			if (GeometryCounters.bOverflowed) Diagnostics.bOverflowed = true;
			if (!bHit) return;
			SaturatingAdd(Work.RawHits, 1);
			Candidate.ActorHandle = Body.Handle;
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(Delta) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		});
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
			if (IsIgnored(Body.Handle, Filter))
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
			bool bHit = false;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.Pair.OverlapCapsuleBox");
				bHit = CollisionGeometry::OverlapCapsuleBox(
					Shape, Transform, Body.Desc.Shape, Body.Desc.Transform, Hit, &GeometryCounters);
			}
			SaturatingAdd(Work.GeometryDistanceEvaluations, GeometryCounters.DistanceEvaluations);
			SaturatingAdd(Work.GeometrySearchIterations, GeometryCounters.SearchIterations);
			if (GeometryCounters.bOverflowed) Diagnostics.bOverflowed = true;
			if (!bHit) continue;
			SaturatingAdd(Work.RawHits, 1);
			Hit.ActorHandle = Body.Handle;
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
		ForEachProductionCandidate([&](const FBodyRecord& Body) {
			SaturatingAdd(Work.BodyVisits, 1);
			SaturatingAdd(Work.Candidates, 1);
			if (IsIgnored(Body.Handle, Filter))
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
			bool bHit = false;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Aether.Query.Pair.OverlapCapsuleBox");
				bHit = CollisionGeometry::OverlapCapsuleBox(
					Shape, Transform, Body.Desc.Shape, Body.Desc.Transform, Hit, &GeometryCounters);
			}
			SaturatingAdd(Work.GeometryDistanceEvaluations, GeometryCounters.DistanceEvaluations);
			SaturatingAdd(Work.GeometrySearchIterations, GeometryCounters.SearchIterations);
			if (GeometryCounters.bOverflowed) Diagnostics.bOverflowed = true;
			if (!bHit) return;
			SaturatingAdd(Work.RawHits, 1);
			Hit.ActorHandle = Body.Handle;
			Hit.Response = Response;
			Hit.UserToken = Body.Desc.UserToken;
			OutHits.push_back(Hit);
		});
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
		SaturatingAdd(Target.RawHits, Source.RawHits);
		SaturatingAdd(Target.ReturnedResults, Source.ReturnedResults);
		SaturatingAdd(Target.Fallbacks, Source.Fallbacks);
		SaturatingAdd(Target.CompareMismatches, Source.CompareMismatches);
		Target.ScratchHighWater = std::max(Target.ScratchHighWater, Source.ScratchHighWater);
		Target.CaptureHighWater = std::max(Target.CaptureHighWater, Source.CaptureHighWater);
		SaturatingAdd(Target.DetailedTimingSamples, Source.DetailedTimingSamples);
		SaturatingAdd(Target.DetailedTimingNanoseconds, Source.DetailedTimingNanoseconds);
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
		const auto It = std::ranges::find(Bodies, Handle, &FBodyRecord::Handle);
		return It != Bodies.end() ? &*It : nullptr;
	}

	auto FPhysicsScene::FindBody(FPhysicsActorHandle Handle) const -> const FBodyRecord*
	{
		const auto It = std::ranges::find(Bodies, Handle, &FBodyRecord::Handle);
		return It != Bodies.end() ? &*It : nullptr;
	}
}
