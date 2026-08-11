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
		uint64 NarrowPhaseLeafTests = 0;
		uint64 GeometryFeatureTests = 0;
		uint64 AssetNodeTests = 0;
		uint64 AssetLeafTests = 0;
		uint64 CompoundChildrenTested = 0;
		uint64 AnalyticDispatches = 0;
		uint64 GenericDispatches = 0;
		uint64 GeometrySupportEvaluations = 0;
		uint64 UnsupportedPairs = 0;
		uint64 NonConvergedPairs = 0;
		uint64 ReferencePairFallbacks = 0;
		uint64 RawHits = 0;
		uint64 ReturnedResults = 0;
		uint64 Fallbacks = 0;
		uint64 CompareMismatches = 0;
		uint64 ScratchHighWater = 0;
		uint64 CaptureHighWater = 0;
		uint64 NodeTests = 0;
		uint64 BoundTests = 0;
		uint64 PrunedNodes = 0;
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
		uint64 SlotReuses = 0;
		uint64 DenseSwaps = 0;
		uint64 StaticBodies = 0;
		uint64 KinematicBodies = 0;
		uint64 DynamicBodies = 0;
		uint64 StaticBuilds = 0;
		uint64 MovingInsertions = 0;
		uint64 MovingRemovals = 0;
		uint64 MovingReinsertions = 0;
		uint64 MovingContainedUpdates = 0;
		uint64 MovingRefits = 0;
		uint64 MovingTreeRebuilds = 0;
		uint64 BoundBuilds = 0;
		uint64 MotionMigrations = 0;
		uint64 FilterOnlyUpdates = 0;
		uint64 SpatialFallbacks = 0;
		uint64 ScratchOverflows = 0;
		uint64 RetainedSpatialBytes = 0;
		uint64 UniqueGeometryResources = 0;
		uint64 RetainedGeometryBytes = 0;
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
			CorruptFirstResult,
			ForceScratchOverflow
		};

		// Dense body payload retaining the owning slot and exact outward-rounded bound.
		struct FBodyRecord
		{
			FPhysicsBodyDesc Desc;
			std::array<float, 6> Bounds{};
			uint32 Slot = 0;
		};

		// Resolves one generation-checked identity or links one free-list entry.
		struct FSlot
		{
			uint32 DenseOrNext = std::numeric_limits<uint32>::max();
			uint32 Generation = 1;
			uint32 ProxyParent = std::numeric_limits<uint32>::max();
		};

		// Compact internal binary-tree node; leaves are encoded stable slot references.
		struct FSpatialNode
		{
			std::array<float, 6> Bounds{};
			uint32 Parent = std::numeric_limits<uint32>::max();
			uint32 Left = std::numeric_limits<uint32>::max();
			uint32 Right = std::numeric_limits<uint32>::max();
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
		auto AccumulateGeometryCounters(
			FPhysicsSceneQueryCounters& Target,
			const CollisionGeometry::FCollisionGeometryCounters& Source) const -> void;
		auto SaturatingAdd(uint64& Target, uint64 Delta) const -> void;
		auto CountMutation(uint64& Counter) -> void;
		auto BuildBodyBounds(const FPhysicsBodyDesc& Desc, std::array<float, 6>& OutBounds) const -> bool;
		auto BuildQueryBounds(
			const FCollisionShape& Shape,
			const FTransform& Transform,
			const FVector3& Delta,
			std::array<float, 6>& OutBounds) const -> bool;
		auto InsertMovingProxy(uint32 Slot) -> bool;
		auto RemoveMovingProxy(uint32 Slot) -> void;
		auto RebuildMovingTree() -> bool;
		auto RebuildStaticTree() const -> bool;
		auto RefreshSpatialDiagnostics() -> void;
		auto IsValidMotionType(EPhysicsBodyMotionType MotionType) const -> bool;
		auto GetHandle(const FBodyRecord& Body) const -> FPhysicsActorHandle;
		auto GetBodyBySlot(uint32 Slot) const -> const FBodyRecord*;
		auto GetBodyBySlot(uint32 Slot) -> FBodyRecord*;
		auto RetainGeometry(const FCollisionGeometryRef& Geometry) -> void;
		auto ReleaseGeometry(const FCollisionGeometryRef& Geometry) -> void;

		template <typename Visitor>
		auto TraverseProductionCandidates(const std::array<float, 6>& QueryBounds,
			const FVector3& SweepStartCenter,
			const FVector3& SweepHalfExtent,
			const FVector3& SweepDelta,
			const FPhysicsQueryHit* BestHit,
			Visitor&& Visit, FPhysicsSceneQueryCounters& Work) const -> bool
		{
			constexpr uint32 Invalid = std::numeric_limits<uint32>::max();
			constexpr uint32 LeafBit = 1u << 31;
			if (!RebuildStaticTree()) return false;
			if (ProductionTestFault == EProductionTestFault::ForceScratchOverflow) return false;
			std::array<std::pair<bool, uint32>, 128> Stack{};
			size_t StackSize = 0;
			auto Push = [&](bool bMoving, uint32 Reference) -> bool
			{
				if (Reference == Invalid) return true;
				if (StackSize == Stack.size()) return false;
				Stack[StackSize++] = {bMoving, Reference};
				Work.ScratchHighWater = std::max<uint64>(Work.ScratchHighWater, StackSize);
				return true;
			};
			if (!Push(false, StaticRoot) || !Push(true, MovingRoot)) return false;
			bool bSkippedFaultCandidate = false;
			while (StackSize)
			{
				const auto [bMoving, Reference] = Stack[--StackSize];
				const std::array<float, 6>* Bounds = nullptr;
				const FBodyRecord* Body = nullptr;
				if ((Reference & LeafBit) != 0)
				{
					Body = GetBodyBySlot(Reference & ~LeafBit);
					if (!Body) continue;
					Bounds = &Body->Bounds;
				}
				else
				{
					const std::vector<FSpatialNode>& Nodes = bMoving ? MovingNodes : StaticNodes;
					if (Reference >= Nodes.size()) continue;
					Bounds = &Nodes[Reference].Bounds;
				}
				SaturatingAdd(Work.BoundTests, 1);
				const bool bIntersects = (*Bounds)[0] <= QueryBounds[3] && (*Bounds)[3] >= QueryBounds[0]
					&& (*Bounds)[1] <= QueryBounds[4] && (*Bounds)[4] >= QueryBounds[1]
					&& (*Bounds)[2] <= QueryBounds[5] && (*Bounds)[5] >= QueryBounds[2];
				if (!bIntersects)
				{
					if ((Reference & LeafBit) == 0) SaturatingAdd(Work.PrunedNodes, 1);
					continue;
				}
				if (BestHit && BestHit->IsHit())
				{
					double NearTime = 0.0;
					double FarTime = 1.0;
					bool bPathIntersects = true;
					for (uint32 Axis = 0; Axis < 3; ++Axis)
					{
						const double Lower = static_cast<double>((*Bounds)[Axis]) - SweepHalfExtent[Axis];
						const double Upper = static_cast<double>((*Bounds)[Axis + 3]) + SweepHalfExtent[Axis];
						if (std::abs(SweepDelta[Axis]) <= 1.0e-12)
						{
							if (SweepStartCenter[Axis] < Lower || SweepStartCenter[Axis] > Upper)
								bPathIntersects = false;
							continue;
						}
						double A = (Lower - SweepStartCenter[Axis]) / SweepDelta[Axis];
						double B = (Upper - SweepStartCenter[Axis]) / SweepDelta[Axis];
						if (A > B) std::swap(A, B);
						NearTime = std::max(NearTime, A);
						FarTime = std::min(FarTime, B);
						if (NearTime > FarTime) bPathIntersects = false;
					}
					if (!bPathIntersects || NearTime > BestHit->Time)
					{
						SaturatingAdd(Work.PrunedNodes, 1);
						continue;
					}
				}
				if (Body)
				{
					if (ProductionTestFault == EProductionTestFault::OmitFirstCandidate && !bSkippedFaultCandidate)
					{
						bSkippedFaultCandidate = true;
						continue;
					}
					Visit(*Body);
					continue;
				}
				SaturatingAdd(Work.NodeTests, 1);
				const FSpatialNode& Node = (bMoving ? MovingNodes : StaticNodes)[Reference];
				const bool bReverse = ProductionTestFault == EProductionTestFault::ReverseCandidates;
				if (!Push(bMoving, bReverse ? Node.Left : Node.Right)
					|| !Push(bMoving, bReverse ? Node.Right : Node.Left)) return false;
			}
			return true;
		}

		std::thread::id OwningThread;
		std::vector<FBodyRecord> Bodies;
		std::unordered_map<uint64, std::pair<uint64, uint64>> GeometryResources;
		std::vector<FSlot> Slots;
		uint32 FreeSlot = std::numeric_limits<uint32>::max();
		std::vector<FSpatialNode> MovingNodes;
		uint32 FreeMovingNode = std::numeric_limits<uint32>::max();
		uint32 MovingRoot = std::numeric_limits<uint32>::max();
		mutable std::vector<FSpatialNode> StaticNodes;
		mutable uint32 StaticRoot = std::numeric_limits<uint32>::max();
		mutable bool bStaticTreeDirty = false;
		EPhysicsSceneQueryExecutionPolicy QueryExecutionPolicy = EPhysicsSceneQueryExecutionPolicy::Production;
		EProductionTestFault ProductionTestFault = EProductionTestFault::None;
		mutable FPhysicsSceneQueryDiagnostics Diagnostics;

		friend struct FPhysicsSceneQueryTestAccess;
	};
}
