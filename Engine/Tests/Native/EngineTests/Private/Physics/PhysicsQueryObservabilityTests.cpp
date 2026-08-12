#include "Collision/CollisionGeometry.h"
#include "Math/Operations.h"
#include "Physics/PhysicsScene.h"
#include "PhysicsQueryTestAccess.h"

#include <gtest/gtest.h>

namespace
{
	constexpr Durin::uint64 FixtureSeed = 0xA37E'2026'0811'0001ull;
	constexpr std::array<size_t, 4> FixtureBodyCounts{0, 32, 1'000, 10'000};

	enum class EFixtureDistribution
	{
		Sparse,
		SparseClosestHit,
		Dense,
		Filtered
	};

	struct FDeterministicGenerator
	{
		Durin::uint64 State = FixtureSeed;

		auto NextUnit() -> double
		{
			State = State * 6'364'136'223'846'793'005ull + 1'442'695'040'888'963'407ull;
			return static_cast<double>(State >> 11) * (1.0 / 9'007'199'254'740'992.0);
		}
	};

	auto MakeBoxBody(
		const Durin::FVector3& Center,
		Durin::uint64 UserToken,
		const Durin::FVector3& HalfExtent = Durin::FVector3(0.5)) -> Durin::FPhysicsBodyDesc
	{
		Durin::FPhysicsBodyDesc Desc;
		Desc.Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
			Durin::FCollisionShape::MakeBox(HalfExtent));
		Desc.Transform = Durin::FTransform();
		Desc.Transform.Translation = Center;
		Desc.UserToken = UserToken;
		return Desc;
	}

	auto AddDeterministicBodies(
		Durin::FPhysicsScene& Scene,
		size_t BodyCount,
		EFixtureDistribution Distribution,
		Durin::uint64 Seed = FixtureSeed) -> std::vector<Durin::FPhysicsActorHandle>
	{
		FDeterministicGenerator Generator{Seed};
		std::vector<Durin::FPhysicsActorHandle> Handles;
		Handles.reserve(BodyCount);
		for (size_t Index = 0; Index < BodyCount; ++Index)
		{
			Durin::FVector3 Center;
			switch (Distribution)
			{
			case EFixtureDistribution::Sparse:
			case EFixtureDistribution::SparseClosestHit:
			case EFixtureDistribution::Filtered:
				Center = {
					100.0 + static_cast<double>(Index % 100) * 4.0,
					-200.0 + static_cast<double>((Index / 100) % 100) * 4.0,
					20.0 + static_cast<double>(Index / 10'000) * 4.0};
				break;
			case EFixtureDistribution::Dense:
				Center = {
					(Generator.NextUnit() - 0.5) * 0.4,
					(Generator.NextUnit() - 0.5) * 0.4,
					(Generator.NextUnit() - 0.5) * 0.4};
				break;
			}

			if (Distribution == EFixtureDistribution::SparseClosestHit && Index == 0) Center = {0.0, 0.0, 0.0};
			Durin::FPhysicsBodyDesc Desc = MakeBoxBody(Center, static_cast<Durin::uint64>(Index + 1));
			Desc.Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
				Generator.NextUnit() * 180.0, Durin::FVectorConstants::Up);
			Desc.Transform.Scale3D = {
				0.5 + Generator.NextUnit() * 1.5,
				0.5 + Generator.NextUnit() * 1.5,
				0.5 + Generator.NextUnit() * 1.5};
			if (Distribution == EFixtureDistribution::Filtered)
			{
				switch (Index % 3)
				{
				case 0:
					Desc.Filter.Responses[0] = Durin::EPhysicsQueryResponse::Ignore;
					break;
				case 1:
					Desc.Filter.Responses[0] = Durin::EPhysicsQueryResponse::Overlap;
					break;
				default:
					break;
				}
			}
			Handles.push_back(Scene.AddBody(Desc));
		}
		return Handles;
	}

	auto ExpectVectorNear(
		const Durin::FVector3& Actual,
		const Durin::FVector3& Expected,
		double Tolerance) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}

	auto ExpectClearedHit(const Durin::FPhysicsQueryHit& Hit) -> void
	{
		EXPECT_FALSE(Hit.ActorHandle.IsValid());
		EXPECT_EQ(Hit.Response, Durin::EPhysicsQueryResponse::Ignore);
		EXPECT_DOUBLE_EQ(Hit.Time, 1.0);
		EXPECT_DOUBLE_EQ(Hit.Distance, 0.0);
		ExpectVectorNear(Hit.Location, Durin::FVector3(0.0), 0.0);
		ExpectVectorNear(Hit.ImpactPoint, Durin::FVector3(0.0), 0.0);
		ExpectVectorNear(Hit.ImpactNormal, Durin::FVector3(0.0), 0.0);
		EXPECT_DOUBLE_EQ(Hit.PenetrationDepth, 0.0);
		EXPECT_EQ(Hit.UserToken, 0u);
		EXPECT_FALSE(Hit.bStartPenetrating);
	}

	auto MakeSeededHit() -> Durin::FPhysicsQueryHit
	{
		Durin::FPhysicsQueryHit Hit;
		Hit.ActorHandle = {99, 7};
		Hit.Response = Durin::EPhysicsQueryResponse::Block;
		Hit.Time = 0.25;
		Hit.Distance = 17.0;
		Hit.Location = {1.0, 2.0, 3.0};
		Hit.ImpactPoint = {4.0, 5.0, 6.0};
		Hit.ImpactNormal = {0.0, 0.0, 1.0};
		Hit.PenetrationDepth = 8.0;
		Hit.UserToken = 123;
		Hit.bStartPenetrating = true;
		return Hit;
	}

	auto ExpectHitsEqual(
		const Durin::FPhysicsQueryHit& Actual,
		const Durin::FPhysicsQueryHit& Expected) -> void
	{
		EXPECT_EQ(Actual.ActorHandle, Expected.ActorHandle);
		EXPECT_EQ(Actual.Response, Expected.Response);
		EXPECT_NEAR(Actual.Time, Expected.Time, 1.0e-12);
		EXPECT_NEAR(Actual.Distance, Expected.Distance, 1.0e-8);
		ExpectVectorNear(Actual.Location, Expected.Location, 1.0e-8);
		ExpectVectorNear(Actual.ImpactPoint, Expected.ImpactPoint, 1.0e-8);
		ExpectVectorNear(Actual.ImpactNormal, Expected.ImpactNormal, 1.0e-8);
		EXPECT_NEAR(Actual.PenetrationDepth, Expected.PenetrationDepth, 1.0e-8);
		EXPECT_EQ(Actual.UserToken, Expected.UserToken);
		EXPECT_EQ(Actual.bStartPenetrating, Expected.bStartPenetrating);
	}

	auto GetQueryCounters(
		const Durin::FPhysicsSceneQueryDiagnostics& Diagnostics,
		Durin::EPhysicsSceneQueryKind QueryKind) -> const Durin::FPhysicsSceneQueryCounters&
	{
		return Diagnostics.Queries[static_cast<size_t>(QueryKind)];
	}

	auto ExpectQueryCountersReconcile(const Durin::FPhysicsSceneQueryCounters& Counters) -> void
	{
		ASSERT_GE(Counters.SubmittedQueries, Counters.InvalidQueries + Counters.OffThreadQueries);
		const Durin::uint64 ValidSubmissions =
			Counters.SubmittedQueries - Counters.InvalidQueries - Counters.OffThreadQueries;
		EXPECT_EQ(
			Counters.ReferenceExecutions + Counters.ProductionExecutions,
			ValidSubmissions + Counters.CompareExecutions);
		EXPECT_EQ(Counters.BodyVisits, Counters.Candidates);
		EXPECT_EQ(
			Counters.Candidates,
			Counters.IgnoredBodies + Counters.FilterRejectedBodies + Counters.NarrowPhasePairTests);
		EXPECT_LE(Counters.RawHits, Counters.NarrowPhasePairTests);
		EXPECT_LE(Counters.ReturnedResults, Counters.RawHits);
		EXPECT_GE(Counters.Fallbacks, Counters.CompareMismatches);
	}

	auto PrintStructuralBaseline(
		std::string_view Fixture,
		size_t BodyCount,
		const Durin::FPhysicsSceneQueryDiagnostics& Diagnostics,
		Durin::EPhysicsSceneQueryKind QueryKind) -> void
	{
		const Durin::FPhysicsSceneQueryCounters& Counters = GetQueryCounters(Diagnostics, QueryKind);
		ExpectQueryCountersReconcile(Counters);
		std::cout << "AetherStructuralBaseline"
			<< ",fixture=" << Fixture
			<< ",bodies=" << BodyCount
			<< ",submitted=" << Counters.SubmittedQueries
			<< ",body_visits=" << Counters.BodyVisits
			<< ",candidates=" << Counters.Candidates
			<< ",ignored=" << Counters.IgnoredBodies
			<< ",filter_rejected=" << Counters.FilterRejectedBodies
			<< ",pair_tests=" << Counters.NarrowPhasePairTests
			<< ",distance_evaluations=" << Counters.GeometryDistanceEvaluations
			<< ",search_iterations=" << Counters.GeometrySearchIterations
			<< ",raw_hits=" << Counters.RawHits
			<< ",returned=" << Counters.ReturnedResults
			<< ",scratch_high_water=" << Counters.ScratchHighWater
			<< ",capture_high_water=" << Counters.CaptureHighWater
			<< ",node_tests=" << Counters.NodeTests
			<< ",bound_tests=" << Counters.BoundTests
			<< ",pruned_nodes=" << Counters.PrunedNodes
			<< ",retained_spatial_bytes=" << Diagnostics.Mutations.RetainedSpatialBytes
			<< ",spatial_fallbacks=" << Diagnostics.Mutations.SpatialFallbacks << '\n';
	}

	template <typename Function>
	auto MeasureOperation(
		std::string_view RecordName,
		std::string_view Fixture,
		size_t BodyCount,
		Durin::uint32 Iterations,
		Function&& Query) -> void
	{
		constexpr Durin::uint32 WarmupCount = 3;
		constexpr Durin::uint32 SampleCount = 11;
		Durin::uint64 Checksum = 0;
		auto Run = [&]() {
			for (Durin::uint32 Iteration = 0; Iteration < Iterations; ++Iteration)
			{
				Checksum += Query() ? 3u : 1u;
			}
		};
		for (Durin::uint32 Warmup = 0; Warmup < WarmupCount; ++Warmup) Run();

		std::vector<Durin::uint64> Samples;
		Samples.reserve(SampleCount);
		for (Durin::uint32 Sample = 0; Sample < SampleCount; ++Sample)
		{
			const auto Start = std::chrono::steady_clock::now();
			Run();
			const Durin::uint64 Elapsed = static_cast<Durin::uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - Start).count());
			Samples.push_back(Elapsed / Iterations);
		}
		std::ranges::sort(Samples);
		const Durin::uint64 Median = Samples[SampleCount / 2];
		const Durin::uint64 P95 = Samples[SampleCount - 1];
		std::cout << RecordName
			<< ",fixture=" << Fixture
			<< ",bodies=" << BodyCount
			<< ",iterations=" << Iterations
			<< ",warmups=" << WarmupCount
			<< ",samples=" << SampleCount
			<< ",median_ns_per_query=" << Median
			<< ",p95_ns_per_query=" << P95
			<< ",checksum=" << Checksum << '\n';
		EXPECT_GT(Checksum, 0u);
	}

	template <typename Function>
	auto MeasureQuery(
		std::string_view Fixture,
		size_t BodyCount,
		Durin::uint32 Iterations,
		Function&& Query) -> void
	{
		MeasureOperation(
			"AetherPrePipelineBaseline", Fixture, BodyCount, Iterations, std::forward<Function>(Query));
	}
}

TEST(FAetherQueryCharacterizationTests, LineTraceFreezesValidationClearingAndEveryResultField)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle Handle = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 41, {1.0, 1.0, 1.0}));
	ASSERT_TRUE(Handle.IsValid());

	Durin::FPhysicsQueryHit Hit = MakeSeededHit();
	ASSERT_TRUE(Scene.LineTraceSingle({-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, Handle);
	EXPECT_EQ(Hit.Response, Durin::EPhysicsQueryResponse::Block);
	EXPECT_NEAR(Hit.Time, 1.0 / 3.0, 1.0e-12);
	EXPECT_NEAR(Hit.Distance, 2.0, 1.0e-12);
	ExpectVectorNear(Hit.Location, {-1.0, 0.0, 0.0}, 1.0e-12);
	ExpectVectorNear(Hit.ImpactPoint, {-1.0, 0.0, 0.0}, 1.0e-12);
	ExpectVectorNear(Hit.ImpactNormal, {-1.0, 0.0, 0.0}, 1.0e-12);
	EXPECT_DOUBLE_EQ(Hit.PenetrationDepth, 0.0);
	EXPECT_EQ(Hit.UserToken, 41u);
	EXPECT_FALSE(Hit.bStartPenetrating);

	ASSERT_TRUE(Scene.LineTraceSingle({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, Handle);
	EXPECT_DOUBLE_EQ(Hit.Time, 0.0);
	EXPECT_DOUBLE_EQ(Hit.Distance, 0.0);
	ExpectVectorNear(Hit.Location, {0.0, 0.0, 0.0}, 0.0);
	ExpectVectorNear(Hit.ImpactPoint, {0.0, 0.0, 0.0}, 0.0);
	ExpectVectorNear(Hit.ImpactNormal, {1.0, 0.0, 0.0}, 0.0);
	EXPECT_DOUBLE_EQ(Hit.PenetrationDepth, 1.0);
	EXPECT_TRUE(Hit.bStartPenetrating);

	Hit = MakeSeededHit();
	EXPECT_FALSE(Scene.LineTraceSingle({3.0, 3.0, 3.0}, {3.0, 3.0, 3.0}, {}, Hit));
	ExpectClearedHit(Hit);
	Hit = MakeSeededHit();
	EXPECT_FALSE(Scene.LineTraceSingle(
		{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, {1.0, 0.0, 0.0}, {}, Hit));
	ExpectClearedHit(Hit);
	Durin::FPhysicsQueryFilter InvalidFilter;
	InvalidFilter.QueryChannel = Durin::MaximumPhysicsChannels;
	Hit = MakeSeededHit();
	EXPECT_FALSE(Scene.LineTraceSingle({-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, InvalidFilter, Hit));
	ExpectClearedHit(Hit);
}

TEST(FAetherQueryCharacterizationTests, SweepFreezesValidationClearingAndPenetrationFields)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle Handle = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 42, {0.5, 3.0, 3.0}));
	ASSERT_TRUE(Handle.IsValid());
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	Durin::FTransform StartTransform;
	StartTransform.Translation = {-3.0, 0.0, 0.0};
	Durin::FPhysicsQueryHit Hit = MakeSeededHit();
	ASSERT_TRUE(Scene.SweepSingle(Capsule, StartTransform, {6.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, Handle);
	EXPECT_EQ(Hit.Response, Durin::EPhysicsQueryResponse::Block);
	EXPECT_NEAR(Hit.Time, 0.35, 2.0e-3);
	EXPECT_NEAR(Hit.Distance, 6.0 * Hit.Time, 1.0e-12);
	ExpectVectorNear(Hit.Location, StartTransform.Translation + Durin::FVector3(6.0, 0.0, 0.0) * Hit.Time, 1.0e-12);
	EXPECT_LT(Hit.ImpactNormal.x, -0.99);
	EXPECT_NEAR(Durin::Math::Length(Hit.ImpactNormal), 1.0, 1.0e-8);
	EXPECT_DOUBLE_EQ(Hit.PenetrationDepth, 0.0);
	EXPECT_EQ(Hit.UserToken, 42u);
	EXPECT_FALSE(Hit.bStartPenetrating);

	StartTransform.Translation = {0.0, 0.0, 0.0};
	ASSERT_TRUE(Scene.SweepSingle(Capsule, StartTransform, {0.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, Handle);
	EXPECT_DOUBLE_EQ(Hit.Time, 0.0);
	EXPECT_DOUBLE_EQ(Hit.Distance, 0.0);
	ExpectVectorNear(Hit.Location, StartTransform.Translation, 0.0);
	EXPECT_GT(Hit.PenetrationDepth, 0.0);
	EXPECT_TRUE(Hit.bStartPenetrating);

	Hit = MakeSeededHit();
	EXPECT_FALSE(Scene.SweepSingle({}, StartTransform, {1.0, 0.0, 0.0}, {}, Hit));
	ExpectClearedHit(Hit);
	StartTransform.Scale3D = {1.0, 0.0, 1.0};
	Hit = MakeSeededHit();
	EXPECT_FALSE(Scene.SweepSingle(Capsule, StartTransform, {1.0, 0.0, 0.0}, {}, Hit));
	ExpectClearedHit(Hit);
}

TEST(FAetherQueryCharacterizationTests, FilteringAndOverlapOrderingRemainTwoSidedAndHandleStable)
{
	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc Block = MakeBoxBody({0.0, 0.0, 0.0}, 51, {1.0, 1.0, 1.0});
	Block.Filter.ObjectChannel = 1;
	Durin::FPhysicsBodyDesc BodyOverlap = Block;
	BodyOverlap.UserToken = 52;
	BodyOverlap.Filter.Responses[0] = Durin::EPhysicsQueryResponse::Overlap;
	const Durin::FPhysicsActorHandle BlockHandle = Scene.AddBody(Block);
	const Durin::FPhysicsActorHandle BodyOverlapHandle = Scene.AddBody(BodyOverlap);
	ASSERT_TRUE(BlockHandle.IsValid());
	ASSERT_TRUE(BodyOverlapHandle.IsValid());

	Durin::FPhysicsQueryFilter Filter;
	Filter.Responses[1] = Durin::EPhysicsQueryResponse::Overlap;
	Durin::FPhysicsQueryHit Single = MakeSeededHit();
	EXPECT_FALSE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Filter, Single));
	ExpectClearedHit(Single);

	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.5, 1.0);
	Durin::FTransform Transform;
	std::vector<Durin::FPhysicsQueryHit> Hits{MakeSeededHit()};
	ASSERT_TRUE(Scene.OverlapMulti(Capsule, Transform, Filter, Hits));
	ASSERT_EQ(Hits.size(), 2u);
	EXPECT_EQ(Hits[0].ActorHandle, BlockHandle);
	EXPECT_EQ(Hits[0].Response, Durin::EPhysicsQueryResponse::Overlap);
	EXPECT_EQ(Hits[0].UserToken, 51u);
	EXPECT_EQ(Hits[1].ActorHandle, BodyOverlapHandle);
	EXPECT_EQ(Hits[1].Response, Durin::EPhysicsQueryResponse::Overlap);
	EXPECT_EQ(Hits[1].UserToken, 52u);
	for (const Durin::FPhysicsQueryHit& Hit : Hits)
	{
		EXPECT_DOUBLE_EQ(Hit.Time, 0.0);
		EXPECT_DOUBLE_EQ(Hit.Distance, 0.0);
		ExpectVectorNear(Hit.Location, Transform.Translation, 0.0);
		EXPECT_TRUE(Hit.bStartPenetrating);
	}

	Filter.IgnoredActors.push_back(BlockHandle);
	ASSERT_TRUE(Scene.OverlapMulti(Capsule, Transform, Filter, Hits));
	ASSERT_EQ(Hits.size(), 1u);
	EXPECT_EQ(Hits.front().ActorHandle, BodyOverlapHandle);
	Filter.QueryChannel = Durin::MaximumPhysicsChannels;
	Hits = {MakeSeededHit()};
	EXPECT_FALSE(Scene.OverlapMulti(Capsule, Transform, Filter, Hits));
	EXPECT_TRUE(Hits.empty());
}

TEST(FAetherQueryCharacterizationTests, EveryQueryRejectsOffThreadAndClearsOutputs)
{
	Durin::FPhysicsScene Scene;
	ASSERT_TRUE(Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 61)).IsValid());
	Durin::FPhysicsQueryHit TraceHit = MakeSeededHit();
	Durin::FPhysicsQueryHit SweepHit = MakeSeededHit();
	std::vector<Durin::FPhysicsQueryHit> OverlapHits{MakeSeededHit()};
	bool bTrace = true;
	bool bSweep = true;
	bool bOverlap = true;
	std::thread Worker([&] {
		bTrace = Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, TraceHit);
		Durin::FTransform Transform;
		const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.5, 1.0);
		bSweep = Scene.SweepSingle(Capsule, Transform, {1.0, 0.0, 0.0}, {}, SweepHit);
		bOverlap = Scene.OverlapMulti(Capsule, Transform, {}, OverlapHits);
	});
	Worker.join();
	EXPECT_FALSE(bTrace);
	EXPECT_FALSE(bSweep);
	EXPECT_FALSE(bOverlap);
	ExpectClearedHit(TraceHit);
	ExpectClearedHit(SweepHit);
	EXPECT_TRUE(OverlapHits.empty());
}

TEST(FAetherQueryCharacterizationTests, TieBreakingAndTangentOverlapSemanticsAreExplicit)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle First = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 71));
	const Durin::FPhysicsActorHandle Second = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 72));
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Second.IsValid());
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, First);
	Durin::FTransform SweepStart;
	SweepStart.Translation = {-2.0, 0.0, 0.0};
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.5, 1.0);
	ASSERT_TRUE(Scene.SweepSingle(Capsule, SweepStart, {4.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, First);

	Durin::FTransform Tangent;
	Tangent.Translation = {1.0, 0.0, 0.0};
	std::vector<Durin::FPhysicsQueryHit> Hits{MakeSeededHit()};
	EXPECT_FALSE(Scene.OverlapMulti(Capsule, Tangent, {}, Hits));
	EXPECT_TRUE(Hits.empty());
}

TEST(FAetherQueryPipelineTests, PoliciesMatchAndOnlyOwningThreadCanSelectAValidPolicy)
{
	Durin::FPhysicsScene Scene;
	EXPECT_EQ(Scene.GetQueryExecutionPolicy(), Durin::EPhysicsSceneQueryExecutionPolicy::Production);
	const Durin::FPhysicsActorHandle First = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 81));
	const Durin::FPhysicsActorHandle Second = Scene.AddBody(MakeBoxBody({2.0, 0.0, 0.0}, 82));
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Second.IsValid());

	Durin::FPhysicsQueryHit ReferenceTrace;
	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Reference));
	ASSERT_TRUE(Scene.LineTraceSingle({-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, {}, ReferenceTrace));
	Durin::FTransform SweepStart;
	SweepStart.Translation = {-3.0, 0.0, 0.0};
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	Durin::FPhysicsQueryHit ReferenceSweep;
	ASSERT_TRUE(Scene.SweepSingle(Capsule, SweepStart, {6.0, 0.0, 0.0}, {}, ReferenceSweep));
	Durin::FTransform OverlapTransform;
	std::vector<Durin::FPhysicsQueryHit> ReferenceOverlaps;
	ASSERT_TRUE(Scene.OverlapMulti(Capsule, OverlapTransform, {}, ReferenceOverlaps));

	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Production));
	Durin::FPhysicsQueryHit ProductionTrace;
	Durin::FPhysicsQueryHit ProductionSweep;
	std::vector<Durin::FPhysicsQueryHit> ProductionOverlaps;
	ASSERT_TRUE(Scene.LineTraceSingle({-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, {}, ProductionTrace));
	ASSERT_TRUE(Scene.SweepSingle(Capsule, SweepStart, {6.0, 0.0, 0.0}, {}, ProductionSweep));
	ASSERT_TRUE(Scene.OverlapMulti(Capsule, OverlapTransform, {}, ProductionOverlaps));
	ExpectHitsEqual(ProductionTrace, ReferenceTrace);
	ExpectHitsEqual(ProductionSweep, ReferenceSweep);
	ASSERT_EQ(ProductionOverlaps.size(), ReferenceOverlaps.size());
	for (size_t Index = 0; Index < ReferenceOverlaps.size(); ++Index)
		ExpectHitsEqual(ProductionOverlaps[Index], ReferenceOverlaps[Index]);

	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Compare));
	Durin::FPhysicsQueryHit CompareTrace;
	Durin::FPhysicsQueryHit CompareSweep;
	std::vector<Durin::FPhysicsQueryHit> CompareOverlaps;
	ASSERT_TRUE(Scene.LineTraceSingle({-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, {}, CompareTrace));
	ASSERT_TRUE(Scene.SweepSingle(Capsule, SweepStart, {6.0, 0.0, 0.0}, {}, CompareSweep));
	ASSERT_TRUE(Scene.OverlapMulti(Capsule, OverlapTransform, {}, CompareOverlaps));
	ExpectHitsEqual(CompareTrace, ReferenceTrace);
	ExpectHitsEqual(CompareSweep, ReferenceSweep);
	ASSERT_EQ(CompareOverlaps.size(), ReferenceOverlaps.size());
	for (size_t Index = 0; Index < ReferenceOverlaps.size(); ++Index)
		ExpectHitsEqual(CompareOverlaps[Index], ReferenceOverlaps[Index]);
	EXPECT_EQ(Durin::FPhysicsSceneQueryTestAccess::GetMismatchCount(Scene), 0u);

	EXPECT_FALSE(Scene.SetQueryExecutionPolicy(static_cast<Durin::EPhysicsSceneQueryExecutionPolicy>(255)));
	EXPECT_EQ(Scene.GetQueryExecutionPolicy(), Durin::EPhysicsSceneQueryExecutionPolicy::Compare);
	bool bOffThreadChanged = true;
	std::thread Worker([&] {
		bOffThreadChanged = Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Reference);
	});
	Worker.join();
	EXPECT_FALSE(bOffThreadChanged);
	EXPECT_EQ(Scene.GetQueryExecutionPolicy(), Durin::EPhysicsSceneQueryExecutionPolicy::Compare);
}

TEST(FAetherQueryPipelineTests, CompareFallsBackToReferenceAndDetectsInjectedDivergence)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle First = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 91));
	const Durin::FPhysicsActorHandle Second = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 92));
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Second.IsValid());
	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Compare));
	ASSERT_TRUE(Scene.SetDetailedQueryDiagnosticsEnabled(true));

	Durin::FPhysicsQueryHit Hit;
	Durin::FPhysicsSceneQueryTestAccess::ReverseCandidates(Scene);
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, First);
	EXPECT_EQ(Durin::FPhysicsSceneQueryTestAccess::GetMismatchCount(Scene), 0u);

	Durin::FPhysicsSceneQueryTestAccess::OmitFirstCandidate(Scene);
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, First);
	EXPECT_EQ(Hit.UserToken, 91u);
	EXPECT_EQ(Durin::FPhysicsSceneQueryTestAccess::GetMismatchCount(Scene), 1u);
	EXPECT_NE(Durin::FPhysicsSceneQueryTestAccess::GetLastDifferenceMask(Scene), 0u);

	Durin::FTransform Transform;
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.5, 1.0);
	std::vector<Durin::FPhysicsQueryHit> Hits;
	Durin::FPhysicsSceneQueryTestAccess::ReverseResults(Scene);
	ASSERT_TRUE(Scene.OverlapMulti(Capsule, Transform, {}, Hits));
	ASSERT_EQ(Hits.size(), 2u);
	EXPECT_EQ(Hits[0].ActorHandle, First);
	EXPECT_EQ(Hits[1].ActorHandle, Second);
	EXPECT_EQ(Durin::FPhysicsSceneQueryTestAccess::GetMismatchCount(Scene), 2u);

	Durin::FPhysicsSceneQueryTestAccess::CorruptFirstResult(Scene);
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, First);
	EXPECT_EQ(Hit.UserToken, 91u);
	EXPECT_EQ(Durin::FPhysicsSceneQueryTestAccess::GetMismatchCount(Scene), 3u);
	Durin::FPhysicsSceneQueryTestAccess::ClearFault(Scene);
}

TEST(FAetherGeometryCounterTests, OptionalSinkCountsBoundedDistanceWorkAndSaturates)
{
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	const Durin::FCollisionShape Box = Durin::FCollisionShape::MakeBox({0.5, 3.0, 3.0});
	Durin::FTransform BoxTransform;
	Durin::FTransform CapsuleTransform;
	Durin::FPhysicsQueryHit CountedHit;
	Durin::CollisionGeometry::FCollisionGeometryCounters Counters;
	ASSERT_TRUE(Durin::CollisionGeometry::OverlapCapsuleBox(
		Capsule, CapsuleTransform, Box, BoxTransform, CountedHit, &Counters));
	EXPECT_EQ(Counters.DistanceEvaluations, 59u);
	EXPECT_EQ(Counters.SearchIterations, 28u);
	EXPECT_FALSE(Counters.bOverflowed);

	Durin::FPhysicsQueryHit UncountedHit;
	ASSERT_TRUE(Durin::CollisionGeometry::OverlapCapsuleBox(
		Capsule, CapsuleTransform, Box, BoxTransform, UncountedHit));
	ExpectHitsEqual(UncountedHit, CountedHit);

	CapsuleTransform.Translation = {-3.0, 0.0, 0.0};
	Counters = {};
	EXPECT_FALSE(Durin::CollisionGeometry::SweepCapsuleBox(
		Capsule, CapsuleTransform, {0.0, 0.0, 0.0}, Box, BoxTransform, CountedHit, &Counters));
	EXPECT_EQ(Counters.DistanceEvaluations, 3'422u);
	EXPECT_EQ(Counters.SearchIterations, 1'652u);

	Counters = {
		.DistanceEvaluations = std::numeric_limits<Durin::uint64>::max() - 10,
		.SearchIterations = std::numeric_limits<Durin::uint64>::max() - 10};
	CapsuleTransform.Translation = {0.0, 0.0, 0.0};
	ASSERT_TRUE(Durin::CollisionGeometry::OverlapCapsuleBox(
		Capsule, CapsuleTransform, Box, BoxTransform, CountedHit, &Counters));
	EXPECT_EQ(Counters.DistanceEvaluations, std::numeric_limits<Durin::uint64>::max());
	EXPECT_EQ(Counters.SearchIterations, std::numeric_limits<Durin::uint64>::max());
	EXPECT_TRUE(Counters.bOverflowed);
}

TEST(FAetherGeometryCounterTests, ProductionCapsuleBoxAvoidsNestedReferenceSearch)
{
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	const Durin::FCollisionShape Box = Durin::FCollisionShape::MakeBox({0.5, 3.0, 3.0});
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::MakePrimitive(Box);
	Durin::FTransform CapsuleTransform;
	CapsuleTransform.Translation = {-3.0, 0.0, 0.0};
	Durin::FPhysicsQueryHit Hit;
	Durin::CollisionGeometry::FCollisionGeometryCounters Counters;
	EXPECT_EQ(Durin::CollisionGeometry::Sweep(
		Capsule, CapsuleTransform, {0.0, 0.0, 0.0}, Geometry, Durin::FTransform(),
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit, &Counters),
		Durin::CollisionGeometry::ECollisionQueryStatus::Miss);
	EXPECT_LE(Counters.DistanceEvaluations, 96u);
	EXPECT_LE(Counters.SearchIterations, 64u);
	EXPECT_EQ(Counters.ReferenceFallbacks, 0u);
}

TEST(FAetherQueryDiagnosticsTests, ReconcilesProductionReferenceAndCompareStructuralWork)
{
	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc IgnoredBody = MakeBoxBody({0.0, 0.0, 0.0}, 101);
	Durin::FPhysicsBodyDesc FilteredBody = MakeBoxBody({0.0, 0.0, 0.0}, 102);
	FilteredBody.Filter.Responses[0] = Durin::EPhysicsQueryResponse::Ignore;
	Durin::FPhysicsBodyDesc HitBody = MakeBoxBody({0.0, 0.0, 0.0}, 103);
	const Durin::FPhysicsActorHandle IgnoredHandle = Scene.AddBody(IgnoredBody);
	ASSERT_TRUE(IgnoredHandle.IsValid());
	ASSERT_TRUE(Scene.AddBody(FilteredBody).IsValid());
	const Durin::FPhysicsActorHandle HitHandle = Scene.AddBody(HitBody);
	ASSERT_TRUE(HitHandle.IsValid());
	Durin::FPhysicsQueryFilter Filter;
	Filter.IgnoredActors.push_back(IgnoredHandle);
	Durin::FPhysicsQueryHit Hit;

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Filter, Hit));
	EXPECT_EQ(Hit.ActorHandle, HitHandle);
	Durin::FPhysicsSceneQueryDiagnostics Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& Production =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	ExpectQueryCountersReconcile(Production);
	EXPECT_EQ(Production.SubmittedQueries, 1u);
	EXPECT_EQ(Production.ProductionExecutions, 1u);
	EXPECT_EQ(Production.BodyVisits, 3u);
	EXPECT_EQ(Production.IgnoredBodies, 1u);
	EXPECT_EQ(Production.FilterRejectedBodies, 1u);
	EXPECT_EQ(Production.NarrowPhasePairTests, 1u);
	EXPECT_EQ(Production.RawHits, 1u);
	EXPECT_EQ(Production.ReturnedResults, 1u);
	EXPECT_EQ(Production.GeometryDistanceEvaluations, 0u);
	EXPECT_TRUE(Snapshot.LastQuery.bValid);
	EXPECT_EQ(Snapshot.LastQuery.QueryKind, Durin::EPhysicsSceneQueryKind::LineTraceSingle);

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Reference));
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Filter, Hit));
	Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& Reference =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	ExpectQueryCountersReconcile(Reference);
	EXPECT_EQ(Reference.ReferenceExecutions, 1u);
	EXPECT_EQ(Reference.BodyVisits, 3u);

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Compare));
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Filter, Hit));
	Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& Compare =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	ExpectQueryCountersReconcile(Compare);
	EXPECT_EQ(Compare.ReferenceExecutions, 1u);
	EXPECT_EQ(Compare.ProductionExecutions, 1u);
	EXPECT_EQ(Compare.CompareExecutions, 1u);
	EXPECT_EQ(Compare.BodyVisits, 6u);
	EXPECT_EQ(Compare.IgnoredBodies, 2u);
	EXPECT_EQ(Compare.FilterRejectedBodies, 2u);
	EXPECT_EQ(Compare.NarrowPhasePairTests, 2u);
	EXPECT_EQ(Compare.RawHits, 2u);
	EXPECT_EQ(Compare.ReturnedResults, 1u);
	EXPECT_LE(Compare.ScratchHighWater, 128u);
	EXPECT_EQ(Compare.CaptureHighWater, 1u);

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	Durin::FTransform CapsuleTransform;
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	ASSERT_TRUE(Scene.SweepSingle(Capsule, CapsuleTransform, {0.0, 0.0, 0.0}, Filter, Hit));
	Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& Sweep =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::SweepSingle);
	ExpectQueryCountersReconcile(Sweep);
	EXPECT_EQ(Sweep.NarrowPhasePairTests, 2u);
	EXPECT_LE(Sweep.GeometryDistanceEvaluations, 96u);
	EXPECT_LE(Sweep.GeometrySearchIterations, 64u);
	EXPECT_EQ(Sweep.RawHits, 2u);

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	std::vector<Durin::FPhysicsQueryHit> Hits;
	ASSERT_TRUE(Scene.OverlapMulti(Capsule, CapsuleTransform, Filter, Hits));
	Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& Overlap =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::OverlapMulti);
	ExpectQueryCountersReconcile(Overlap);
	EXPECT_EQ(Overlap.ReturnedResults, 1u);
	EXPECT_EQ(Overlap.RawHits, 2u);
	EXPECT_LE(Overlap.ScratchHighWater, 128u);
}

TEST(FAetherQueryDiagnosticsTests, ReconcilesRejectedQueriesDetailedMismatchAndSaturation)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle Handle = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 111));
	ASSERT_TRUE(Handle.IsValid());
	Durin::FPhysicsQueryHit Hit = MakeSeededHit();

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	EXPECT_FALSE(Scene.LineTraceSingle(
		{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, {1.0, 0.0, 0.0}, {}, Hit));
	Durin::FPhysicsSceneQueryDiagnostics Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& Invalid =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	ExpectQueryCountersReconcile(Invalid);
	EXPECT_EQ(Invalid.SubmittedQueries, 1u);
	EXPECT_EQ(Invalid.InvalidQueries, 1u);
	EXPECT_FALSE(Snapshot.LastQuery.bInputValid);

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	bool bOffThreadResult = true;
	std::thread Worker([&] {
		bOffThreadResult = Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit);
	});
	Worker.join();
	EXPECT_FALSE(bOffThreadResult);
	Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& OffThread =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	ExpectQueryCountersReconcile(OffThread);
	EXPECT_EQ(OffThread.OffThreadQueries, 1u);
	EXPECT_TRUE(Snapshot.LastQuery.bOffThreadRejected);

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Compare));
	Durin::FPhysicsSceneQueryTestAccess::OmitFirstCandidate(Scene);
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& MismatchWithoutDetail =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	ExpectQueryCountersReconcile(MismatchWithoutDetail);
	EXPECT_EQ(MismatchWithoutDetail.CompareMismatches, 1u);
	EXPECT_EQ(MismatchWithoutDetail.Fallbacks, 1u);
	EXPECT_FALSE(Snapshot.LastMismatch.bValid);
	EXPECT_EQ(MismatchWithoutDetail.DetailedTimingSamples, 0u);

	ASSERT_TRUE(Scene.SetDetailedQueryDiagnosticsEnabled(true));
	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneQueryCounters& MismatchWithDetail =
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	ExpectQueryCountersReconcile(MismatchWithDetail);
	EXPECT_EQ(MismatchWithDetail.DetailedTimingSamples, 1u);
	EXPECT_TRUE(Snapshot.LastMismatch.bValid);
	EXPECT_NE(Snapshot.LastMismatch.DifferenceMask, 0u);
	EXPECT_EQ(Hit.ActorHandle, Handle);

	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	Durin::FPhysicsSceneQueryTestAccess::SaturateSubmittedQueries(Scene);
	EXPECT_FALSE(Scene.LineTraceSingle(
		{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, {1.0, 0.0, 0.0}, {}, Hit));
	Snapshot = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(
		GetQueryCounters(Snapshot, Durin::EPhysicsSceneQueryKind::LineTraceSingle).SubmittedQueries,
		std::numeric_limits<Durin::uint64>::max());
	EXPECT_TRUE(Snapshot.bOverflowed);
	Durin::FPhysicsSceneQueryTestAccess::ClearFault(Scene);
}

TEST(FAetherQueryDiagnosticsTests, MutationCountersAndResetRetainAConstantTimeBodyBaseline)
{
	Durin::FPhysicsScene Scene;
	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	Durin::FPhysicsBodyDesc Desc = MakeBoxBody({0.0, 0.0, 0.0}, 121);
	const Durin::FPhysicsActorHandle Handle = Scene.AddBody(Desc);
	ASSERT_TRUE(Handle.IsValid());
	EXPECT_FALSE(Scene.AddBody({}).IsValid());
	Desc.Transform.Translation = {1.0, 0.0, 0.0};
	EXPECT_TRUE(Scene.UpdateBody(Handle, Desc));
	EXPECT_FALSE(Scene.UpdateBody({999, 1}, Desc));
	EXPECT_TRUE(Scene.RemoveBody(Handle));
	EXPECT_FALSE(Scene.RemoveBody(Handle));
	Durin::FPhysicsSceneQueryDiagnostics Snapshot = Scene.CaptureQueryDiagnostics();
	const Durin::FPhysicsSceneMutationCounters& Mutations = Snapshot.Mutations;
	EXPECT_EQ(Mutations.AddCalls, 2u);
	EXPECT_EQ(Mutations.AddSuccesses, 1u);
	EXPECT_EQ(Mutations.AddRejected, 1u);
	EXPECT_EQ(Mutations.UpdateCalls, 2u);
	EXPECT_EQ(Mutations.UpdateSuccesses, 1u);
	EXPECT_EQ(Mutations.UpdateRejected, 1u);
	EXPECT_EQ(Mutations.RemoveCalls, 2u);
	EXPECT_EQ(Mutations.RemoveSuccesses, 1u);
	EXPECT_EQ(Mutations.RemoveRejected, 1u);
	EXPECT_EQ(Mutations.FailedLookups, 2u);
	EXPECT_EQ(Mutations.BodiesAtReset, 0u);
	EXPECT_EQ(Mutations.BodiesPresent, 0u);
	EXPECT_EQ(Mutations.AddCalls, Mutations.AddSuccesses + Mutations.AddRejected);
	EXPECT_EQ(Mutations.UpdateCalls, Mutations.UpdateSuccesses + Mutations.UpdateRejected);
	EXPECT_EQ(Mutations.RemoveCalls, Mutations.RemoveSuccesses + Mutations.RemoveRejected);

	AddDeterministicBodies(Scene, 10'000, EFixtureDistribution::Sparse);
	ASSERT_TRUE(Scene.SetDetailedQueryDiagnosticsEnabled(true));
	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	Snapshot = Scene.CaptureQueryDiagnostics();
	EXPECT_TRUE(Snapshot.bDetailedDiagnosticsEnabled);
	EXPECT_EQ(Snapshot.Mutations.BodiesAtReset, 10'000u);
	EXPECT_EQ(Snapshot.Mutations.BodiesPresent, 10'000u);
	EXPECT_EQ(Snapshot.Mutations.AddCalls, 0u);
	EXPECT_FALSE(Snapshot.LastQuery.bValid);
	EXPECT_FALSE(Snapshot.LastMismatch.bValid);

	Durin::FPhysicsSceneQueryDiagnostics OffThreadSnapshot;
	std::thread Worker([&] { OffThreadSnapshot = Scene.CaptureQueryDiagnostics(); });
	Worker.join();
	EXPECT_EQ(OffThreadSnapshot.Mutations.BodiesPresent, 0u);
	EXPECT_FALSE(OffThreadSnapshot.LastQuery.bValid);
}

TEST(FAetherSceneAccelerationTests, ReusesGenerationSlotsAndRepairsDenseSwapsWithoutChangingIdentity)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle First = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 1));
	const Durin::FPhysicsActorHandle Middle = Scene.AddBody(MakeBoxBody({2.0, 0.0, 0.0}, 2));
	const Durin::FPhysicsActorHandle Last = Scene.AddBody(MakeBoxBody({4.0, 0.0, 0.0}, 3));
	ASSERT_TRUE(First.IsValid());
	ASSERT_TRUE(Middle.IsValid());
	ASSERT_TRUE(Last.IsValid());
	ASSERT_TRUE(Scene.RemoveBody(Middle));
	EXPECT_FALSE(Scene.ContainsBody(Middle));
	EXPECT_TRUE(Scene.ContainsBody(First));
	EXPECT_TRUE(Scene.ContainsBody(Last));
	const std::vector<Durin::FPhysicsBodySnapshot> AfterSwap = Scene.CaptureBodies();
	ASSERT_EQ(AfterSwap.size(), 2u);
	EXPECT_EQ(AfterSwap[0].Handle, First);
	EXPECT_EQ(AfterSwap[1].Handle, Last);

	const Durin::FPhysicsActorHandle Reused = Scene.AddBody(MakeBoxBody({2.0, 0.0, 0.0}, 4));
	EXPECT_EQ(Reused.Id, Middle.Id);
	EXPECT_GT(Reused.Generation, Middle.Generation);
	EXPECT_FALSE(Scene.ContainsBody(Middle));
	EXPECT_TRUE(Scene.ContainsBody(Reused));
	EXPECT_FALSE(Scene.UpdateBody(Middle, MakeBoxBody({8.0, 0.0, 0.0}, 5)));
	EXPECT_FALSE(Scene.RemoveBody(Middle));
	const Durin::FPhysicsSceneMutationCounters Mutations = Scene.CaptureQueryDiagnostics().Mutations;
	EXPECT_EQ(Mutations.SlotReuses, 1u);
	EXPECT_EQ(Mutations.DenseSwaps, 1u);
}

TEST(FAetherSceneAccelerationTests, BuildsConservativeBoundsForEveryShapeAndRejectsOverflow)
{
	static_assert(static_cast<Durin::uint8>(Durin::EPhysicsBodyMotionType::Static) == 0);
	static_assert(static_cast<Durin::uint8>(Durin::EPhysicsBodyMotionType::Kinematic) == 1);
	static_assert(static_cast<Durin::uint8>(Durin::EPhysicsBodyMotionType::Dynamic) == 2);
	EXPECT_EQ(Durin::FPhysicsBodyDesc{}.MotionType, Durin::EPhysicsBodyMotionType::Kinematic);

	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc Box = MakeBoxBody({10.0, 20.0, 30.0}, 1, {2.0, 1.0, 0.5});
	Box.Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(90.0, Durin::FVectorConstants::Up);
	const Durin::FPhysicsActorHandle BoxHandle = Scene.AddBody(Box);
	ASSERT_TRUE(BoxHandle.IsValid());
	const auto BoxBounds = Durin::FPhysicsSceneQueryTestAccess::GetBodyBounds(Scene, BoxHandle);
	EXPECT_LT(BoxBounds[0], 9.49f);
	EXPECT_GT(BoxBounds[3], 10.51f);
	EXPECT_LT(BoxBounds[1], 18.0f);
	EXPECT_GT(BoxBounds[4], 22.0f);

	Durin::FPhysicsBodyDesc Sphere;
	Sphere.Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
		Durin::FCollisionShape::MakeSphere(2.0));
	Sphere.Transform.Scale3D = {1.0, 2.0, 3.0};
	const auto SphereHandle = Scene.AddBody(Sphere);
	ASSERT_TRUE(SphereHandle.IsValid());
	const auto SphereBounds = Durin::FPhysicsSceneQueryTestAccess::GetBodyBounds(Scene, SphereHandle);
	EXPECT_LT(SphereBounds[0], -6.0f);
	EXPECT_GT(SphereBounds[3], 6.0f);

	Durin::FPhysicsBodyDesc Capsule;
	Capsule.Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
		Durin::FCollisionShape::MakeCapsule(1.0, 3.0));
	Capsule.Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(90.0, Durin::FVectorConstants::Right);
	const auto CapsuleHandle = Scene.AddBody(Capsule);
	ASSERT_TRUE(CapsuleHandle.IsValid());
	const auto CapsuleBounds = Durin::FPhysicsSceneQueryTestAccess::GetBodyBounds(Scene, CapsuleHandle);
	EXPECT_LT(CapsuleBounds[0], -3.0f);
	EXPECT_GT(CapsuleBounds[3], 3.0f);

	Durin::FPhysicsBodyDesc Overflow = Box;
	Overflow.Transform.Translation.x = std::numeric_limits<double>::max();
	EXPECT_FALSE(Scene.AddBody(Overflow).IsValid());
}

TEST(FAetherSceneAccelerationTests, MaintainsMotionPartitionsAndIsolatesMovingUpdatesFromStaticBuilds)
{
	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc Static = MakeBoxBody({0.0, 0.0, 0.0}, 1);
	Static.MotionType = Durin::EPhysicsBodyMotionType::Static;
	Durin::FPhysicsBodyDesc Moving = MakeBoxBody({3.0, 0.0, 0.0}, 2);
	Durin::FPhysicsBodyDesc Dynamic = MakeBoxBody({6.0, 0.0, 0.0}, 3);
	Dynamic.MotionType = Durin::EPhysicsBodyMotionType::Dynamic;
	ASSERT_TRUE(Scene.AddBody(Static).IsValid());
	const Durin::FPhysicsActorHandle MovingHandle = Scene.AddBody(Moving);
	ASSERT_TRUE(MovingHandle.IsValid());
	ASSERT_TRUE(Scene.AddBody(Dynamic).IsValid());
	Durin::FPhysicsQueryHit Hit;
	EXPECT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {8.0, 0.0, 0.0}, {}, Hit));
	ASSERT_TRUE(Scene.ResetQueryDiagnostics());

	Moving.UserToken = 22;
	ASSERT_TRUE(Scene.UpdateBody(MovingHandle, Moving));
	Moving.Transform.Translation.x += 0.05;
	ASSERT_TRUE(Scene.UpdateBody(MovingHandle, Moving));
	EXPECT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {8.0, 0.0, 0.0}, {}, Hit));
	auto Mutations = Scene.CaptureQueryDiagnostics().Mutations;
	EXPECT_EQ(Mutations.StaticBuilds, 0u);
	EXPECT_EQ(Mutations.MovingRemovals, 0u);
	EXPECT_EQ(Mutations.FilterOnlyUpdates, 1u);
	EXPECT_EQ(Mutations.MovingContainedUpdates, 1u);

	Moving.Transform.Translation.x = 30.0;
	ASSERT_TRUE(Scene.UpdateBody(MovingHandle, Moving));
	Mutations = Scene.CaptureQueryDiagnostics().Mutations;
	EXPECT_EQ(Mutations.MovingReinsertions, 1u);
	EXPECT_EQ(Mutations.MovingRemovals, 1u);
	Moving.MotionType = Durin::EPhysicsBodyMotionType::Static;
	ASSERT_TRUE(Scene.UpdateBody(MovingHandle, Moving));
	Mutations = Scene.CaptureQueryDiagnostics().Mutations;
	EXPECT_EQ(Mutations.MotionMigrations, 1u);
	EXPECT_EQ(Mutations.StaticBodies, 2u);
	EXPECT_EQ(Mutations.KinematicBodies, 0u);
	EXPECT_EQ(Mutations.DynamicBodies, 1u);
}

TEST(FAetherSceneAccelerationTests, ScratchOverflowFallsBackToTheCompleteReferenceResult)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle Handle = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, 77));
	ASSERT_TRUE(Handle.IsValid());
	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Compare));
	Durin::FPhysicsSceneQueryTestAccess::ForceScratchOverflow(Scene);
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, Handle);
	const Durin::FPhysicsSceneQueryDiagnostics Diagnostics = Scene.CaptureQueryDiagnostics();
	const auto& Counters = GetQueryCounters(Diagnostics, Durin::EPhysicsSceneQueryKind::LineTraceSingle);
	EXPECT_EQ(Counters.Fallbacks, 1u);
	EXPECT_EQ(Counters.CompareMismatches, 0u);
	EXPECT_EQ(Diagnostics.Mutations.SpatialFallbacks, 1u);
	EXPECT_EQ(Diagnostics.Mutations.ScratchOverflows, 1u);
	Durin::FPhysicsSceneQueryTestAccess::ClearFault(Scene);
}

TEST(FAetherQueryParityTests, FixedSeedRandomizedAdversarialScenesRemainMismatchFreeThroughChurn)
{
	constexpr Durin::uint64 ParitySeed = 0xA37E'5041'5459'0001ull;
	for (size_t Scenario = 0; Scenario < 16; ++Scenario)
	{
		const Durin::uint64 ScenarioSeed = ParitySeed + static_cast<Durin::uint64>(Scenario);
		SCOPED_TRACE(std::format("seed={}, scenario={}", ScenarioSeed, Scenario));
		FDeterministicGenerator Generator{ScenarioSeed};
		std::vector<Durin::FPhysicsBodyDesc> Descs;
		Descs.reserve(32);
		for (size_t Index = 0; Index < 32; ++Index)
		{
			Durin::FPhysicsBodyDesc Desc = MakeBoxBody(
				{
					(Generator.NextUnit() - 0.5) * 20.0,
					(Generator.NextUnit() - 0.5) * 20.0,
					(Generator.NextUnit() - 0.5) * 6.0},
				static_cast<Durin::uint64>(Scenario * 100 + Index + 1),
				{
					0.2 + Generator.NextUnit() * 1.8,
					0.2 + Generator.NextUnit() * 1.8,
					0.2 + Generator.NextUnit() * 1.8});
			Desc.Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
				Generator.NextUnit() * 180.0,
				Index % 2 == 0 ? Durin::FVectorConstants::Up : Durin::FVectorConstants::Right);
			Desc.Transform.Scale3D = {
				0.25 + Generator.NextUnit() * 2.0,
				0.25 + Generator.NextUnit() * 2.0,
				0.25 + Generator.NextUnit() * 2.0};
			Desc.Filter.ObjectChannel = static_cast<Durin::uint8>(Index % 4);
			if (Index % 7 == 0) Desc.Filter.Responses[Scenario % 4] = Durin::EPhysicsQueryResponse::Ignore;
			else if (Index % 7 == 1) Desc.Filter.Responses[Scenario % 4] = Durin::EPhysicsQueryResponse::Overlap;
			Descs.push_back(Desc);
		}

		std::vector<size_t> Order(Descs.size());
		for (size_t Index = 0; Index < Order.size(); ++Index) Order[Index] = Index;
		std::ranges::rotate(Order, Order.begin() + static_cast<std::ptrdiff_t>(Scenario % Order.size()));
		if (Scenario % 2 != 0) std::ranges::reverse(Order);
		Durin::FPhysicsScene Scene;
		std::vector<Durin::FPhysicsActorHandle> Handles;
		Handles.reserve(Order.size());
		for (const size_t Index : Order) Handles.push_back(Scene.AddBody(Descs[Index]));
		ASSERT_TRUE(std::ranges::all_of(Handles, &Durin::FPhysicsActorHandle::IsValid));
		ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Compare));
		if (Scenario % 2 != 0) Durin::FPhysicsSceneQueryTestAccess::ReverseCandidates(Scene);
		ASSERT_TRUE(Scene.ResetQueryDiagnostics());

		Durin::FPhysicsQueryFilter Filter;
		Filter.QueryChannel = static_cast<Durin::uint8>(Scenario % 4);
		if (Scenario % 3 == 0) Filter.Responses[(Scenario + 1) % 4] = Durin::EPhysicsQueryResponse::Overlap;
		for (size_t Index = Scenario % 5; Index < Handles.size(); Index += 5)
			Filter.IgnoredActors.push_back(Handles[Index]);
		Filter.IgnoredActors.push_back({999'999, 7});

		Durin::FPhysicsQueryHit Hit;
		bool bQueryResult = Scene.LineTraceSingle(
			{-12.0, 0.0, 0.0}, {12.0, 0.0, 0.0}, Filter, Hit);
		EXPECT_EQ(bQueryResult, Hit.IsHit());
		bQueryResult = Scene.LineTraceSingle(
			Descs.front().Transform.Translation, Descs.front().Transform.Translation, Filter, Hit);
		EXPECT_EQ(bQueryResult, Hit.IsHit());
		bQueryResult = Scene.LineTraceSingle(
			{0.0, -12.0, 0.0}, {0.0, 12.0, 0.0}, Filter, Hit);
		EXPECT_EQ(bQueryResult, Hit.IsHit());

		const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(
			0.25 + Generator.NextUnit() * 0.75, 1.0 + Generator.NextUnit() * 1.5);
		Durin::FTransform CapsuleTransform;
		CapsuleTransform.Translation = {
			(Generator.NextUnit() - 0.5) * 8.0,
			(Generator.NextUnit() - 0.5) * 8.0,
			(Generator.NextUnit() - 0.5) * 4.0};
		const Durin::FVector3 Delta{
			(Generator.NextUnit() - 0.5) * 12.0,
			(Generator.NextUnit() - 0.5) * 12.0,
			(Generator.NextUnit() - 0.5) * 4.0};
		bQueryResult = Scene.SweepSingle(Capsule, CapsuleTransform, Delta, Filter, Hit);
		EXPECT_EQ(bQueryResult, Hit.IsHit());
		std::vector<Durin::FPhysicsQueryHit> Hits;
		bQueryResult = Scene.OverlapMulti(Capsule, CapsuleTransform, Filter, Hits);
		EXPECT_EQ(bQueryResult, !Hits.empty());

		const size_t RemovedIndex = Scenario % Handles.size();
		EXPECT_TRUE(Scene.RemoveBody(Handles[RemovedIndex]));
		Durin::FPhysicsBodyDesc Updated = Descs[(Scenario + 1) % Descs.size()];
		Updated.Transform.Translation += Durin::FVector3(0.125, -0.25, 0.5);
		const size_t UpdatedIndex = (RemovedIndex + 1) % Handles.size();
		EXPECT_TRUE(Scene.UpdateBody(Handles[UpdatedIndex], Updated));
		EXPECT_TRUE(Scene.AddBody(Descs[(Scenario + 2) % Descs.size()]).IsValid());
		bQueryResult = Scene.SweepSingle(Capsule, CapsuleTransform, -Delta, Filter, Hit);
		EXPECT_EQ(bQueryResult, Hit.IsHit());
		bQueryResult = Scene.OverlapMulti(Capsule, CapsuleTransform, Filter, Hits);
		EXPECT_EQ(bQueryResult, !Hits.empty());

		Hit = MakeSeededHit();
		EXPECT_FALSE(Scene.LineTraceSingle(
			{std::numeric_limits<double>::infinity(), 0.0, 0.0}, {0.0, 0.0, 0.0}, Filter, Hit));
		ExpectClearedHit(Hit);
		Durin::FTransform InvalidTransform;
		InvalidTransform.Scale3D = {-1.0, 1.0, 1.0};
		EXPECT_FALSE(Scene.SweepSingle(Capsule, InvalidTransform, Delta, Filter, Hit));

		const Durin::FPhysicsSceneQueryDiagnostics Diagnostics = Scene.CaptureQueryDiagnostics();
		for (const Durin::FPhysicsSceneQueryCounters& Counters : Diagnostics.Queries)
		{
			ExpectQueryCountersReconcile(Counters);
			EXPECT_EQ(Counters.CompareMismatches, 0u);
		}
		Durin::FPhysicsSceneQueryTestAccess::ClearFault(Scene);
	}
}

TEST(DISABLED_FAetherQueryBaselineBenchmarks, RecordsPrePipelineFlatQueryBaseline)
{
	for (const size_t BodyCount : FixtureBodyCounts)
	{
		SCOPED_TRACE(std::format("seed={}, bodies={}", FixtureSeed, BodyCount));
		Durin::FPhysicsScene SparseScene;
		AddDeterministicBodies(SparseScene, BodyCount, EFixtureDistribution::Sparse);
		Durin::FPhysicsScene HitScene;
		AddDeterministicBodies(HitScene, BodyCount, EFixtureDistribution::SparseClosestHit);
		Durin::FPhysicsScene DenseScene;
		AddDeterministicBodies(DenseScene, BodyCount, EFixtureDistribution::Dense);

		const Durin::uint32 TraceIterations = static_cast<Durin::uint32>(
			std::clamp<size_t>(200'000 / std::max<size_t>(BodyCount, 1), 1, 10'000));
		const Durin::uint32 GeometryIterations = static_cast<Durin::uint32>(
			std::clamp<size_t>(1'000 / std::max<size_t>(BodyCount, 1), 1, 10'000));
		Durin::FPhysicsQueryHit Hit;
		MeasureQuery("line_sparse_miss", BodyCount, TraceIterations, [&] {
			return SparseScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});
		MeasureQuery("line_sparse_closest_hit", BodyCount, TraceIterations, [&] {
			return HitScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});

		const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.5, 1.0);
		Durin::FTransform Transform;
		Transform.Translation = {-10.0, 0.0, 0.0};
		MeasureQuery("sweep_sparse_miss", BodyCount, GeometryIterations, [&] {
			return SparseScene.SweepSingle(Capsule, Transform, {20.0, 0.0, 0.0}, {}, Hit);
		});
		Transform.Translation = {0.0, 0.0, 0.0};
		MeasureQuery("sweep_dense_penetration", BodyCount, GeometryIterations, [&] {
			return DenseScene.SweepSingle(Capsule, Transform, {1.0, 0.0, 0.0}, {}, Hit);
		});
		std::vector<Durin::FPhysicsQueryHit> Hits;
		MeasureQuery("overlap_dense", BodyCount, GeometryIterations, [&] {
			return DenseScene.OverlapMulti(Capsule, Transform, {}, Hits);
		});
	}
}

TEST(DISABLED_FAetherQueryDiagnosticsBenchmarks, RecordsDetailedOverheadAndConstantTimeObservation)
{
	for (const size_t BodyCount : FixtureBodyCounts)
	{
		SCOPED_TRACE(std::format("seed={}, bodies={}", FixtureSeed, BodyCount));
		Durin::FPhysicsScene Scene;
		AddDeterministicBodies(Scene, BodyCount, EFixtureDistribution::Sparse);
		const Durin::uint32 QueryIterations = static_cast<Durin::uint32>(
			std::clamp<size_t>(200'000 / std::max<size_t>(BodyCount, 1), 1, 10'000));
		Durin::FPhysicsQueryHit Hit;
		ASSERT_TRUE(Scene.SetDetailedQueryDiagnosticsEnabled(false));
		ASSERT_TRUE(Scene.ResetQueryDiagnostics());
		MeasureOperation("AetherDiagnosticsCost", "line_detailed_disabled", BodyCount, QueryIterations, [&] {
			return Scene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});

		ASSERT_TRUE(Scene.SetDetailedQueryDiagnosticsEnabled(true));
		ASSERT_TRUE(Scene.ResetQueryDiagnostics());
		MeasureOperation("AetherDiagnosticsCost", "line_detailed_enabled", BodyCount, QueryIterations, [&] {
			return Scene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});

		constexpr Durin::uint32 ObservationIterations = 10'000;
		MeasureOperation("AetherDiagnosticsCost", "capture", BodyCount, ObservationIterations, [&] {
			return Scene.CaptureQueryDiagnostics().Mutations.BodiesPresent == BodyCount;
		});
		MeasureOperation("AetherDiagnosticsCost", "reset", BodyCount, ObservationIterations, [&] {
			return Scene.ResetQueryDiagnostics();
		});
	}
}

TEST(DISABLED_FAetherQueryQualificationBenchmarks, RecordsStageThreeStructureTimingMutationAndMemory)
{
	std::cout << "AetherRetainedMemory"
		<< ",scene_bytes=" << Durin::FPhysicsSceneQueryTestAccess::GetSceneSize()
		<< ",body_record_bytes=" << Durin::FPhysicsSceneQueryTestAccess::GetBodyRecordSize()
		<< ",diagnostics_bytes=" << Durin::FPhysicsSceneQueryTestAccess::GetDiagnosticsSize()
		<< ",mismatch_bytes=" << Durin::FPhysicsSceneQueryTestAccess::GetMismatchSize()
		<< ",mismatch_capacity=1\n";

	for (const size_t BodyCount : FixtureBodyCounts)
	{
		SCOPED_TRACE(std::format("seed={}, bodies={}", FixtureSeed, BodyCount));
		Durin::FPhysicsScene SparseScene;
		const std::vector<Durin::FPhysicsActorHandle> SparseHandles = AddDeterministicBodies(
			SparseScene, BodyCount, EFixtureDistribution::Sparse);
		Durin::FPhysicsScene HitScene;
		AddDeterministicBodies(HitScene, BodyCount, EFixtureDistribution::SparseClosestHit);
		Durin::FPhysicsScene DenseScene;
		AddDeterministicBodies(DenseScene, BodyCount, EFixtureDistribution::Dense);
		Durin::FPhysicsScene FilteredScene;
		AddDeterministicBodies(FilteredScene, BodyCount, EFixtureDistribution::Filtered);

		Durin::FPhysicsQueryHit Hit;
		ASSERT_TRUE(SparseScene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Compare));
		ASSERT_TRUE(SparseScene.ResetQueryDiagnostics());
		SparseScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		const Durin::FCollisionShape ParityCapsule = Durin::FCollisionShape::MakeCapsule(0.5, 1.0);
		Durin::FTransform ParityTransform;
		ParityTransform.Translation = {-10.0, 0.0, 0.0};
		SparseScene.SweepSingle(ParityCapsule, ParityTransform, {20.0, 0.0, 0.0}, {}, Hit);
		std::vector<Durin::FPhysicsQueryHit> ParityHits;
		SparseScene.OverlapMulti(ParityCapsule, ParityTransform, {}, ParityHits);
		const Durin::FPhysicsSceneQueryDiagnostics ParityDiagnostics = SparseScene.CaptureQueryDiagnostics();
		Durin::uint64 ParityMismatches = 0;
		for (const Durin::FPhysicsSceneQueryCounters& Counters : ParityDiagnostics.Queries)
		{
			ExpectQueryCountersReconcile(Counters);
			ParityMismatches += Counters.CompareMismatches;
		}
		EXPECT_EQ(ParityMismatches, 0u);
		std::cout << "AetherScaleParity,bodies=" << BodyCount
			<< ",mismatches=" << ParityMismatches << '\n';
		ASSERT_TRUE(SparseScene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Production));
		ASSERT_TRUE(SparseScene.ResetQueryDiagnostics());
		SparseScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		PrintStructuralBaseline(
			"line_sparse_miss", BodyCount, SparseScene.CaptureQueryDiagnostics(),
			Durin::EPhysicsSceneQueryKind::LineTraceSingle);
		ASSERT_TRUE(HitScene.ResetQueryDiagnostics());
		HitScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		PrintStructuralBaseline(
			"line_sparse_closest_hit", BodyCount, HitScene.CaptureQueryDiagnostics(),
			Durin::EPhysicsSceneQueryKind::LineTraceSingle);
		ASSERT_TRUE(DenseScene.ResetQueryDiagnostics());
		DenseScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		PrintStructuralBaseline(
			"line_dense_crossing", BodyCount, DenseScene.CaptureQueryDiagnostics(),
			Durin::EPhysicsSceneQueryKind::LineTraceSingle);
		ASSERT_TRUE(FilteredScene.ResetQueryDiagnostics());
		FilteredScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		PrintStructuralBaseline(
			"line_filter_mix", BodyCount, FilteredScene.CaptureQueryDiagnostics(),
			Durin::EPhysicsSceneQueryKind::LineTraceSingle);

		Durin::FPhysicsQueryFilter IgnoredFilter;
		for (size_t Index = 0; Index < SparseHandles.size(); Index += 3)
			IgnoredFilter.IgnoredActors.push_back(SparseHandles[Index]);
		ASSERT_TRUE(SparseScene.ResetQueryDiagnostics());
		SparseScene.LineTraceSingle(
			{-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, IgnoredFilter, Hit);
		PrintStructuralBaseline(
			"line_ignored_thirds", BodyCount, SparseScene.CaptureQueryDiagnostics(),
			Durin::EPhysicsSceneQueryKind::LineTraceSingle);

		const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.5, 1.0);
		Durin::FTransform SweepTransform;
		SweepTransform.Translation = {-10.0, 0.0, 0.0};
		ASSERT_TRUE(SparseScene.ResetQueryDiagnostics());
		SparseScene.SweepSingle(Capsule, SweepTransform, {20.0, 0.0, 0.0}, {}, Hit);
		PrintStructuralBaseline(
			"sweep_sparse_miss", BodyCount, SparseScene.CaptureQueryDiagnostics(),
			Durin::EPhysicsSceneQueryKind::SweepSingle);
		Durin::FTransform OverlapTransform;
		std::vector<Durin::FPhysicsQueryHit> Hits;
		ASSERT_TRUE(DenseScene.ResetQueryDiagnostics());
		DenseScene.OverlapMulti(Capsule, OverlapTransform, {}, Hits);
		PrintStructuralBaseline(
			"overlap_dense", BodyCount, DenseScene.CaptureQueryDiagnostics(),
			Durin::EPhysicsSceneQueryKind::OverlapMulti);

		const Durin::uint32 TraceIterations = static_cast<Durin::uint32>(
			std::clamp<size_t>(200'000 / std::max<size_t>(BodyCount, 1), 1, 10'000));
		const Durin::uint32 GeometryIterations = static_cast<Durin::uint32>(
			std::clamp<size_t>(1'000 / std::max<size_t>(BodyCount, 1), 1, 10'000));
		MeasureOperation("AetherStageThreeTiming", "line_sparse_miss", BodyCount, TraceIterations, [&] {
			return SparseScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});
		MeasureOperation("AetherStageThreeTiming", "line_sparse_closest_hit", BodyCount, TraceIterations, [&] {
			return HitScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});
		MeasureOperation("AetherStageThreeTiming", "line_dense_crossing", BodyCount, TraceIterations, [&] {
			return DenseScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});
		MeasureOperation("AetherStageThreeTiming", "line_filter_mix", BodyCount, TraceIterations, [&] {
			return FilteredScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		});
		MeasureOperation("AetherStageThreeTiming", "line_ignored_thirds", BodyCount, TraceIterations, [&] {
			return SparseScene.LineTraceSingle(
				{-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, IgnoredFilter, Hit);
		});
		MeasureOperation("AetherStageThreeTiming", "sweep_sparse_miss", BodyCount, GeometryIterations, [&] {
			return SparseScene.SweepSingle(Capsule, SweepTransform, {20.0, 0.0, 0.0}, {}, Hit);
		});
		MeasureOperation("AetherStageThreeTiming", "overlap_dense", BodyCount, GeometryIterations, [&] {
			return DenseScene.OverlapMulti(Capsule, OverlapTransform, {}, Hits);
		});

		Durin::FPhysicsScene MutationScene;
		std::vector<Durin::FPhysicsActorHandle> MutationHandles = AddDeterministicBodies(
			MutationScene, BodyCount, EFixtureDistribution::Sparse);
		ASSERT_TRUE(MutationScene.ResetQueryDiagnostics());
		Durin::FPhysicsBodyDesc MutationDesc = MakeBoxBody({500.0, 0.0, 0.0}, 0xC0FFEE);
		for (size_t Index = 0; Index < MutationHandles.size(); Index += 3)
			EXPECT_TRUE(MutationScene.RemoveBody(MutationHandles[Index]));
		for (size_t Index = 1; Index < MutationHandles.size(); Index += 3)
			EXPECT_TRUE(MutationScene.UpdateBody(MutationHandles[Index], MutationDesc));
		const size_t RemovedCount = (BodyCount + 2) / 3;
		for (size_t Index = 0; Index < RemovedCount; ++Index)
			EXPECT_TRUE(MutationScene.AddBody(MutationDesc).IsValid());
		const Durin::FPhysicsSceneMutationCounters Mutations =
			MutationScene.CaptureQueryDiagnostics().Mutations;
		std::cout << "AetherMutationBaseline"
			<< ",bodies=" << BodyCount
			<< ",add_calls=" << Mutations.AddCalls
			<< ",updates=" << Mutations.UpdateSuccesses
			<< ",removes=" << Mutations.RemoveSuccesses
			<< ",failed_lookups=" << Mutations.FailedLookups
			<< ",bodies_present=" << Mutations.BodiesPresent << '\n';

		if (BodyCount > 0)
		{
			Durin::FPhysicsScene MutationTimingScene;
			std::vector<Durin::FPhysicsActorHandle> TimingHandles = AddDeterministicBodies(
				MutationTimingScene, BodyCount, EFixtureDistribution::Sparse);
			const Durin::uint32 MutationIterations = static_cast<Durin::uint32>(
				std::clamp<size_t>(100'000 / BodyCount, 1, 10'000));
			MeasureOperation("AetherStageThreeTiming", "update_last", BodyCount, MutationIterations, [&] {
				return MutationTimingScene.UpdateBody(TimingHandles.back(), MutationDesc);
			});
			Durin::FPhysicsActorHandle CurrentLast = TimingHandles.back();
			MeasureOperation("AetherStageThreeTiming", "remove_add_last", BodyCount, MutationIterations, [&] {
				if (!MutationTimingScene.RemoveBody(CurrentLast)) return false;
				CurrentLast = MutationTimingScene.AddBody(MutationDesc);
				return CurrentLast.IsValid();
			});
		}
	}
}

TEST(DISABLED_FAetherNarrowphaseBenchmarks, RecordsCapsuleBoxProductionSpeedup)
{
	constexpr Durin::uint32 SampleCount = 15;
	constexpr Durin::uint32 IterationsPerSample = 200;
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	const Durin::FCollisionShape Box = Durin::FCollisionShape::MakeBox({0.5, 3.0, 3.0});
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::MakePrimitive(Box);
	Durin::FTransform CapsuleTransform;
	CapsuleTransform.Translation = {-3.0, 0.0, 0.0};
	std::array<Durin::uint64, SampleCount> ReferenceSamples{};
	std::array<Durin::uint64, SampleCount> ProductionSamples{};
	for (Durin::uint32 Sample = 0; Sample < SampleCount; ++Sample)
	{
		auto Measure = [&](bool bReference) {
			const auto Start = std::chrono::steady_clock::now();
			for (Durin::uint32 Iteration = 0; Iteration < IterationsPerSample; ++Iteration)
			{
				Durin::FPhysicsQueryHit Hit;
				if (bReference)
					EXPECT_FALSE(Durin::CollisionGeometry::SweepCapsuleBox(
						Capsule, CapsuleTransform, Durin::FVector3(0.0), Box, Durin::FTransform(), Hit));
				else
					EXPECT_EQ(Durin::CollisionGeometry::Sweep(
						Capsule, CapsuleTransform, Durin::FVector3(0.0), Geometry, Durin::FTransform(),
						Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
						Durin::CollisionGeometry::ECollisionQueryStatus::Miss);
			}
			return static_cast<Durin::uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count()) / IterationsPerSample;
		};
		ReferenceSamples[Sample] = Measure(true);
		ProductionSamples[Sample] = Measure(false);
	}
	std::ranges::sort(ReferenceSamples);
	std::ranges::sort(ProductionSamples);
	const Durin::uint64 ReferenceMedian = ReferenceSamples[SampleCount / 2];
	const Durin::uint64 ProductionMedian = ProductionSamples[SampleCount / 2];
	std::cout << "AETHER_M2_CAPSULE_BOX reference_median_ns=" << ReferenceMedian
		<< ",production_median_ns=" << ProductionMedian
		<< ",speedup=" << static_cast<double>(ReferenceMedian) / std::max<Durin::uint64>(1, ProductionMedian)
		<< '\n';
	EXPECT_GT(ReferenceMedian, ProductionMedian * 4);
}
