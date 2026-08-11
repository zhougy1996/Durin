#pragma once

#include "AetherAPI.h"
#include "Physics/PhysicsTypes.h"

namespace Durin
{
	// Selects the retained reference executor, production pipeline, or synchronous parity comparison.
	enum class EPhysicsSceneQueryExecutionPolicy : uint8
	{
		Reference,
		Production,
		Compare
	};

	// Identifies one public scene-query operation in diagnostics.
	enum class EPhysicsSceneQueryKind : uint8
	{
		LineTraceSingle,
		SweepSingle,
		OverlapMulti,
		Count
	};

	// Cumulative structural work for one query kind or one bounded last-query record.
	struct FPhysicsSceneQueryCounters
	{
		uint64 SubmittedQueries = 0;
		uint64 InvalidQueries = 0;
		uint64 OffThreadQueries = 0;
		uint64 ReferenceExecutions = 0;
		uint64 ProductionExecutions = 0;
		uint64 CompareExecutions = 0;
		uint64 BodyVisits = 0;
		uint64 Candidates = 0;
		uint64 IgnoredBodies = 0;
		uint64 FilterRejectedBodies = 0;
		uint64 NarrowPhasePairTests = 0;
		uint64 GeometryDistanceEvaluations = 0;
		uint64 GeometrySearchIterations = 0;
		uint64 RawHits = 0;
		uint64 ReturnedResults = 0;
		uint64 Fallbacks = 0;
		uint64 CompareMismatches = 0;
		uint64 ScratchHighWater = 0;
		uint64 CaptureHighWater = 0;
		uint64 DetailedTimingSamples = 0;
		uint64 DetailedTimingNanoseconds = 0;
	};

	// Fixed mismatch facts for the most recent captured Compare divergence.
	struct FPhysicsSceneQueryMismatch
	{
		bool bValid = false;
		EPhysicsSceneQueryKind QueryKind = EPhysicsSceneQueryKind::LineTraceSingle;
		bool bReferenceStatus = false;
		bool bProductionStatus = false;
		uint32 ReferenceCount = 0;
		uint32 ProductionCount = 0;
		uint32 FirstDifferingResult = 0;
		FPhysicsActorHandle ReferenceWinner;
		FPhysicsActorHandle ProductionWinner;
		uint32 DifferenceMask = 0;
	};

	// Bounded detail for the most recently submitted scene query.
	struct FPhysicsSceneLastQueryDiagnostics
	{
		bool bValid = false;
		EPhysicsSceneQueryKind QueryKind = EPhysicsSceneQueryKind::LineTraceSingle;
		EPhysicsSceneQueryExecutionPolicy Policy = EPhysicsSceneQueryExecutionPolicy::Production;
		bool bInputValid = false;
		bool bOffThreadRejected = false;
		bool bResult = false;
		FPhysicsSceneQueryCounters Counters;
	};

	// Reconciled body-lifecycle values since the last O(1) diagnostic reset.
	struct FPhysicsSceneMutationCounters
	{
		uint64 AddCalls = 0;
		uint64 AddSuccesses = 0;
		uint64 AddRejected = 0;
		uint64 UpdateCalls = 0;
		uint64 UpdateSuccesses = 0;
		uint64 UpdateRejected = 0;
		uint64 RemoveCalls = 0;
		uint64 RemoveSuccesses = 0;
		uint64 RemoveRejected = 0;
		uint64 FailedLookups = 0;
		uint64 BodiesAtReset = 0;
		uint64 BodiesPresent = 0;
	};

	// Value-only snapshot of scene query, mismatch, and mutation diagnostics.
	struct FPhysicsSceneQueryDiagnostics
	{
		std::array<FPhysicsSceneQueryCounters, static_cast<size_t>(EPhysicsSceneQueryKind::Count)> Queries{};
		FPhysicsSceneMutationCounters Mutations;
		FPhysicsSceneLastQueryDiagnostics LastQuery;
		FPhysicsSceneQueryMismatch LastMismatch;
		bool bDetailedDiagnosticsEnabled = false;
		bool bOverflowed = false;
	};

	// Immutable debug copy of one low-level scene entry.
	struct FPhysicsBodySnapshot
	{
		FPhysicsActorHandle Handle;
		FPhysicsBodyDesc Desc;
	};

	// Owns deterministic query-only body state for one World without Engine object pointers.
	class FPhysicsScene
	{
	public:
		AETHER_API FPhysicsScene();
		AETHER_API ~FPhysicsScene();
		FPhysicsScene(const FPhysicsScene&) = delete;
		auto operator=(const FPhysicsScene&) -> FPhysicsScene& = delete;

		AETHER_API auto AddBody(const FPhysicsBodyDesc& Desc) -> FPhysicsActorHandle;
		AETHER_API auto RemoveBody(FPhysicsActorHandle Handle) -> bool;
		AETHER_API auto UpdateBody(FPhysicsActorHandle Handle, const FPhysicsBodyDesc& Desc) -> bool;
		AETHER_API auto ContainsBody(FPhysicsActorHandle Handle) const -> bool;
		AETHER_API auto GetBodyCount() const -> size_t;
		AETHER_API auto CaptureBodies() const -> std::vector<FPhysicsBodySnapshot>;
		AETHER_API auto SetQueryExecutionPolicy(EPhysicsSceneQueryExecutionPolicy Policy) -> bool;
		auto GetQueryExecutionPolicy() const -> EPhysicsSceneQueryExecutionPolicy { return QueryExecutionPolicy; }
		// Enables bounded mismatch payloads and steady-clock sampling on the owning thread.
		AETHER_API auto SetDetailedQueryDiagnosticsEnabled(bool bEnabled) -> bool;
		// Returns an O(1) value copy, or a default snapshot when called off-thread.
		AETHER_API auto CaptureQueryDiagnostics() const -> FPhysicsSceneQueryDiagnostics;
		// Clears cumulative/last-query values in O(1) while retaining the current body baseline.
		AETHER_API auto ResetQueryDiagnostics() -> bool;

		AETHER_API auto LineTraceSingle(
			const FVector3& Start,
			const FVector3& End,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit) const -> bool;
		AETHER_API auto SweepSingle(
			const FCollisionShape& Shape,
			const FTransform& StartTransform,
			const FVector3& Delta,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit) const -> bool;
		AETHER_API auto OverlapMulti(
			const FCollisionShape& Shape,
			const FTransform& Transform,
			const FPhysicsQueryFilter& Filter,
			std::vector<FPhysicsQueryHit>& OutHits) const -> bool;

	private:
		enum class EProductionTestFault : uint8
		{
			None,
			OmitFirstCandidate,
			ReverseCandidates,
			ReverseResults,
			CorruptFirstResult
		};

		struct FBodyRecord
		{
			FPhysicsActorHandle Handle;
			FPhysicsBodyDesc Desc;
		};

		auto IsOwningThread() const -> bool;
		auto FindBody(FPhysicsActorHandle Handle) -> FBodyRecord*;
		auto FindBody(FPhysicsActorHandle Handle) const -> const FBodyRecord*;
		auto IsValidPolicy(EPhysicsSceneQueryExecutionPolicy Policy) const -> bool;
		auto IsLineTraceValid(
			const FVector3& Start,
			const FVector3& End,
			const FPhysicsQueryFilter& Filter) const -> bool;
		auto IsSweepValid(
			const FCollisionShape& Shape,
			const FTransform& StartTransform,
			const FVector3& Delta,
			const FPhysicsQueryFilter& Filter) const -> bool;
		auto IsOverlapValid(
			const FCollisionShape& Shape,
			const FTransform& Transform,
			const FPhysicsQueryFilter& Filter) const -> bool;
		auto ExecuteReferenceLineTrace(
			const FVector3& Start,
			const FVector3& End,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit,
			FPhysicsSceneQueryCounters& Work) const -> bool;
		auto ExecuteProductionLineTrace(
			const FVector3& Start,
			const FVector3& End,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit,
			FPhysicsSceneQueryCounters& Work) const -> bool;
		auto ExecuteReferenceSweep(
			const FCollisionShape& Shape,
			const FTransform& StartTransform,
			const FVector3& Delta,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit,
			FPhysicsSceneQueryCounters& Work) const -> bool;
		auto ExecuteProductionSweep(
			const FCollisionShape& Shape,
			const FTransform& StartTransform,
			const FVector3& Delta,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit,
			FPhysicsSceneQueryCounters& Work) const -> bool;
		auto ExecuteReferenceOverlap(
			const FCollisionShape& Shape,
			const FTransform& Transform,
			const FPhysicsQueryFilter& Filter,
			std::vector<FPhysicsQueryHit>& OutHits,
			FPhysicsSceneQueryCounters& Work) const -> bool;
		auto ExecuteProductionOverlap(
			const FCollisionShape& Shape,
			const FTransform& Transform,
			const FPhysicsQueryFilter& Filter,
			std::vector<FPhysicsQueryHit>& OutHits,
			FPhysicsSceneQueryCounters& Work) const -> bool;
		auto RecordSingleMismatch(
			EPhysicsSceneQueryKind QueryKind,
			bool bReferenceStatus,
			const FPhysicsQueryHit& ReferenceHit,
			bool bProductionStatus,
			const FPhysicsQueryHit& ProductionHit,
			FPhysicsSceneQueryMismatch& OutMismatch) const -> bool;
		auto RecordMultiMismatch(
			bool bReferenceStatus,
			const std::vector<FPhysicsQueryHit>& ReferenceHits,
			bool bProductionStatus,
			const std::vector<FPhysicsQueryHit>& ProductionHits,
			FPhysicsSceneQueryMismatch& OutMismatch) const -> bool;
		auto BeginQuery(EPhysicsSceneQueryKind QueryKind) const -> FPhysicsSceneLastQueryDiagnostics;
		auto BeginDetailedTiming() const -> uint64;
		auto CommitQuery(FPhysicsSceneLastQueryDiagnostics& Query, uint64 StartNanoseconds) const -> void;
		auto MergeQueryCounters(
			FPhysicsSceneQueryCounters& Target,
			const FPhysicsSceneQueryCounters& Source) const -> void;
		auto SaturatingAdd(uint64& Target, uint64 Delta) const -> void;
		auto CountMutation(uint64& Counter) -> void;

		template <typename Visitor>
		auto ForEachProductionCandidate(Visitor&& Visit) const -> void
		{
			if (ProductionTestFault == EProductionTestFault::ReverseCandidates)
			{
				for (auto It = Bodies.rbegin(); It != Bodies.rend(); ++It) Visit(*It);
				return;
			}
			for (size_t Index = 0; Index < Bodies.size(); ++Index)
			{
				if (ProductionTestFault == EProductionTestFault::OmitFirstCandidate && Index == 0) continue;
				Visit(Bodies[Index]);
			}
		}

		std::thread::id OwningThread;
		std::vector<FBodyRecord> Bodies;
		uint64 NextHandleId = 1;
		EPhysicsSceneQueryExecutionPolicy QueryExecutionPolicy = EPhysicsSceneQueryExecutionPolicy::Production;
		EProductionTestFault ProductionTestFault = EProductionTestFault::None;
		mutable FPhysicsSceneQueryDiagnostics Diagnostics;

		friend struct FPhysicsSceneQueryTestAccess;
	};
}
