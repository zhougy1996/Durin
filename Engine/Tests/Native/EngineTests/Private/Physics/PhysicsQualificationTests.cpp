#include "Collision/CollisionGeometry.h"
#include "Math/Operations.h"
#include "Physics/PhysicsScene.h"
#include "PhysicsQueryTestAccess.h"

#include <gtest/gtest.h>
#include <chrono>
#include <random>

namespace
{
	constexpr uint64 FixtureSeed = 0xA37E'2026'0811'0001ull;
	constexpr std::array<size_t, 4> FixtureBodyCounts{0, 32, 1'000, 10'000};
	inline constexpr uint64 ExpectedPrimitiveRetainedBytes =
		DURIN_BUILD_DEBUG ? 208u : 200u;

	enum class EFixtureDistribution
	{
		Sparse,
		Dense
	};

	struct FDeterministicGenerator
	{
		uint64 State = FixtureSeed;

		auto NextUnit() -> double
		{
			State = State * 6'364'136'223'846'793'005ull + 1'442'695'040'888'963'407ull;
			return static_cast<double>(State >> 11) * (1.0 / 9'007'199'254'740'992.0);
		}
	};

	auto MakeBoxBody(
		const Durin::FVector3& Center,
		uint64 UserToken,
		const Durin::FVector3& HalfExtent = Durin::FVector3(0.5)) -> Durin::FPhysicsBodyDesc
	{
		Durin::FPhysicsBodyDesc Desc;
		Desc.Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
			Durin::FCollisionShape::MakeBox(HalfExtent));
		Desc.Transform.Translation = Center;
		Desc.UserToken = UserToken;
		return Desc;
	}

	auto AddDeterministicBodies(
		Durin::FPhysicsScene& Scene,
		size_t BodyCount,
		EFixtureDistribution Distribution) -> std::vector<Durin::FPhysicsActorHandle>
	{
		FDeterministicGenerator Generator;
		std::vector<Durin::FPhysicsActorHandle> Handles;
		Handles.reserve(BodyCount);
		for (size_t Index = 0; Index < BodyCount; ++Index)
		{
			Durin::FVector3 Center;
			if (Distribution == EFixtureDistribution::Sparse)
			{
				Center = {
					100.0 + static_cast<double>(Index % 100) * 4.0,
					-200.0 + static_cast<double>((Index / 100) % 100) * 4.0,
					20.0 + static_cast<double>(Index / 10'000) * 4.0};
			}
			else
			{
				Center = {
					(Generator.NextUnit() - 0.5) * 0.4,
					(Generator.NextUnit() - 0.5) * 0.4,
					(Generator.NextUnit() - 0.5) * 0.4};
			}

			Durin::FPhysicsBodyDesc Desc = MakeBoxBody(Center, static_cast<uint64>(Index + 1));
			Desc.Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
				Generator.NextUnit() * 180.0, Durin::FVectorConstants::Up);
			Desc.Transform.Scale3D = {
				0.5 + Generator.NextUnit() * 1.5,
				0.5 + Generator.NextUnit() * 1.5,
				0.5 + Generator.NextUnit() * 1.5};
			Handles.push_back(Scene.AddBody(Desc));
		}
		return Handles;
	}

	auto GetQueryCounters(
		const Durin::FPhysicsSceneQueryDiagnostics& Diagnostics,
		Durin::EPhysicsSceneQueryKind QueryKind) -> const Durin::FPhysicsSceneQueryCounters&
	{
		return Diagnostics.Queries[static_cast<size_t>(QueryKind)];
	}
}

TEST(FPhysicsCollisionGeometryQualificationTests, TenThousandBodiesRetainOneSharedPayload)
{
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
		Durin::FCollisionShape::MakeBox({0.5, 0.5, 0.5}));
	EXPECT_EQ(Geometry.GetRetainedBytes(), ExpectedPrimitiveRetainedBytes);
	Durin::FPhysicsScene Scene;
	std::vector<Durin::FPhysicsActorHandle> Handles;
	Handles.reserve(10'000);
	for (uint32 Index = 0; Index < 10'000; ++Index)
	{
		Durin::FPhysicsBodyDesc Desc;
		Desc.Geometry = Geometry;
		Desc.Transform.Translation = {static_cast<double>(Index) * 2.0, 0.0, 0.0};
		Handles.push_back(Scene.AddBody(Desc));
		ASSERT_TRUE(Handles.back().IsValid());
	}
	Durin::FPhysicsSceneQueryDiagnostics Diagnostics = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(Diagnostics.Mutations.UniqueGeometryResources, 1u);
	EXPECT_EQ(Diagnostics.Mutations.RetainedGeometryBytes, Geometry.GetRetainedBytes());
	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	Diagnostics = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(Diagnostics.Mutations.UniqueGeometryResources, 1u);
	for (const Durin::FPhysicsActorHandle Handle : Handles) ASSERT_TRUE(Scene.RemoveBody(Handle));
	Diagnostics = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(Diagnostics.Mutations.UniqueGeometryResources, 0u);
	EXPECT_EQ(Diagnostics.Mutations.RetainedGeometryBytes, 0u);
}

TEST(FPhysicsCollisionGeometryQualificationTests, ProductionBvhMatchesReferenceAndKeepsSparseWorkLocal)
{
	constexpr uint32 GridSize = 224;
	std::vector<Durin::FVector3> Vertices;
	std::vector<uint32> Indices;
	Vertices.reserve((GridSize + 1) * (GridSize + 1));
	Indices.reserve(GridSize * GridSize * 6);
	for (uint32 Y = 0; Y <= GridSize; ++Y)
		for (uint32 X = 0; X <= GridSize; ++X)
			Vertices.emplace_back(static_cast<double>(X), static_cast<double>(Y), 0.0);
	for (uint32 Y = 0; Y < GridSize; ++Y)
	{
		for (uint32 X = 0; X < GridSize; ++X)
		{
			const uint32 A = Y * (GridSize + 1) + X;
			const uint32 B = A + 1;
			const uint32 C = A + GridSize + 1;
			const uint32 D = C + 1;
			Indices.insert(Indices.end(), {A, B, D, A, D, C});
		}
	}
	Durin::FCollisionGeometryBuildDiagnostics Facts;
	const Durin::FCollisionGeometryRef Mesh =
		Durin::FCollisionGeometryRef::BuildTriangleMesh(Vertices, Indices, &Facts);
	ASSERT_TRUE(Mesh.IsValid());
	EXPECT_EQ(Facts.RetainedTriangles, 100'352u);
	RecordProperty("grid_node_count", Facts.NodeCount);
	RecordProperty("grid_maximum_depth", Facts.MaximumDepth);
	RecordProperty("grid_retained_bytes", Facts.RetainedBytes);
	RecordProperty("grid_estimated_peak_bytes", Facts.EstimatedPeakBytes);
	EXPECT_LE(Facts.MaximumDepth, 64u);
	EXPECT_EQ(Mesh.GetLeafTriangleCount(), Facts.RetainedTriangles);
	for (uint32 Index = 0; Index < Mesh.GetNodeCount(); ++Index)
	{
		const Durin::FCollisionGeometryNode* Node = Mesh.GetNode(Index);
		ASSERT_NE(Node, nullptr);
		EXPECT_TRUE(Durin::Math::IsFinite(Node->Minimum));
		EXPECT_TRUE(Durin::Math::IsFinite(Node->Maximum));
		if (Node->IsLeaf()) EXPECT_LE(Node->GetLeafCount(), 8u);
	}

	Durin::FPhysicsQueryHit Reference;
	Durin::FPhysicsQueryHit Production;
	Durin::CollisionGeometry::FCollisionGeometryCounters ReferenceWork;
	Durin::CollisionGeometry::FCollisionGeometryCounters ProductionWork;
	EXPECT_EQ(Durin::CollisionGeometry::Raycast(
		{1000.0, 1000.0, -1.0}, {1000.0, 1000.0, 1.0}, Mesh, Durin::FTransform(),
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference, &ReferenceWork),
		Durin::CollisionGeometry::ECollisionQueryStatus::Miss);
	EXPECT_EQ(Durin::CollisionGeometry::Raycast(
		{1000.0, 1000.0, -1.0}, {1000.0, 1000.0, 1.0}, Mesh, Durin::FTransform(),
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production, &ProductionWork),
		Durin::CollisionGeometry::ECollisionQueryStatus::Miss);
	EXPECT_EQ(ReferenceWork.FeatureTests, 100'352u);
	EXPECT_EQ(ProductionWork.AssetNodeTests, 1u);
	EXPECT_EQ(ProductionWork.FeatureTests, 0u);
	EXPECT_EQ(ProductionWork.ReferenceFallbacks, 0u);

	ReferenceWork = {};
	ProductionWork = {};
	ASSERT_EQ(Durin::CollisionGeometry::Raycast(
		{112.25, 112.25, -2.0}, {112.25, 112.25, 2.0}, Mesh, Durin::FTransform(),
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference, &ReferenceWork),
		Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	ASSERT_EQ(Durin::CollisionGeometry::Raycast(
		{112.25, 112.25, -2.0}, {112.25, 112.25, 2.0}, Mesh, Durin::FTransform(),
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production, &ProductionWork),
		Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	EXPECT_DOUBLE_EQ(Reference.Time, Production.Time);
	EXPECT_EQ(Reference.ImpactNormal, Production.ImpactNormal);
	EXPECT_LT(ProductionWork.FeatureTests, 64u);
	EXPECT_LT(ProductionWork.AssetNodeTests, 128u);
	EXPECT_EQ(ProductionWork.ReferenceFallbacks, 0u);
	std::mt19937_64 Random(0x42564832ull);
	std::uniform_real_distribution<double> Coordinate(-10.0, GridSize + 10.0);
	for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
	{
		const double X = Coordinate(Random);
		const double Y = Coordinate(Random);
		ReferenceWork = {};
		ProductionWork = {};
		const auto ReferenceStatus = Durin::CollisionGeometry::Raycast(
			{X, Y, -2.0}, {X, Y, 2.0}, Mesh, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference, &ReferenceWork);
		const auto ProductionStatus = Durin::CollisionGeometry::Raycast(
			{X, Y, -2.0}, {X, Y, 2.0}, Mesh, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production, &ProductionWork);
		EXPECT_EQ(ReferenceStatus, ProductionStatus);
		if (ReferenceStatus == Durin::CollisionGeometry::ECollisionQueryStatus::Hit)
			EXPECT_DOUBLE_EQ(Reference.Time, Production.Time);
		EXPECT_EQ(ProductionWork.ReferenceFallbacks, 0u);
		EXPECT_FALSE(ProductionWork.bOverflowed);
	}
}

TEST(FPhysicsHeightFieldQualificationTests, MeetsMaximumBuildQueryMemoryAndSharingBudgets)
{
	using FClock = std::chrono::steady_clock;
	constexpr uint32 Dimension = 1025;
	constexpr uint32 QueryCount = 256;
	std::vector<uint16> Samples(static_cast<size_t>(Dimension) * Dimension);
	for (uint32 Y = 0; Y < Dimension; ++Y)
		for (uint32 X = 0; X < Dimension; ++X)
			Samples[static_cast<size_t>(Y) * Dimension + X] = static_cast<uint16>(
				(X * 31u + Y * 17u + ((X ^ Y) & 63u) * 257u) & 0xffffu);

	Durin::FCollisionGeometryBuildDiagnostics Facts;
	const auto BuildStart = FClock::now();
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::BuildHeightField(
		Dimension, Dimension, Samples, 1.0, 1.0, 200.0, -100.0, &Facts);
	const auto BuildEnd = FClock::now();
	ASSERT_TRUE(Geometry.IsValid());
	EXPECT_EQ(Facts.NodeCount, 32767u);
	EXPECT_EQ(Facts.MaximumDepth, 15u);
	EXPECT_LT(Facts.RetainedBytes, 4u * 1024u * 1024u);
	EXPECT_LT(Facts.EstimatedPeakBytes, 7u * 1024u * 1024u);

	Durin::CollisionGeometry::FCollisionGeometryCounters Work;
	Durin::FPhysicsQueryHit Hit;
	FDeterministicGenerator Generator;
	const auto QueryStart = FClock::now();
	for (uint32 Index = 0; Index < QueryCount; ++Index)
	{
		const double X = Generator.NextUnit() * static_cast<double>(Dimension - 1);
		const double Y = Generator.NextUnit() * static_cast<double>(Dimension - 1);
		EXPECT_EQ(Durin::CollisionGeometry::Raycast(
			{X, Y, 150.0}, {X, Y, -150.0}, Geometry, {},
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit, &Work),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	}
	const auto QueryEnd = FClock::now();
	EXPECT_EQ(Work.HeightFieldTriangleTests, Work.HeightFieldCellTests * 2u);
	EXPECT_LT(Work.HeightFieldCellTests,
		static_cast<uint64>(QueryCount) * (Dimension - 1) * (Dimension - 1));
	EXPECT_EQ(Work.ReferenceFallbacks, 0u);
	EXPECT_EQ(Work.Unsupported, 0u);
	EXPECT_EQ(Work.NonConverged, 0u);
	EXPECT_FALSE(Work.bOverflowed);
	Durin::CollisionGeometry::FCollisionGeometryCounters DenseReferenceWork;
	const auto ReferenceStart = FClock::now();
	EXPECT_EQ(Durin::CollisionGeometry::Raycast(
		{-1.0, -1.0, 150.0}, {-1.0, -1.0, -150.0}, Geometry, {},
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &DenseReferenceWork),
		Durin::CollisionGeometry::ECollisionQueryStatus::Miss);
	const auto ReferenceEnd = FClock::now();
	EXPECT_EQ(DenseReferenceWork.HeightFieldCellTests,
		static_cast<uint64>(Dimension - 1) * (Dimension - 1));
	EXPECT_EQ(DenseReferenceWork.HeightFieldTriangleTests,
		DenseReferenceWork.HeightFieldCellTests * 2u);

	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc FirstBody;
	FirstBody.Geometry = Geometry;
	const auto FirstBodyStart = FClock::now();
	ASSERT_TRUE(Scene.AddBody(FirstBody).IsValid());
	const auto FirstBodyEnd = FClock::now();
	for (uint32 Index = 1; Index < 1024; ++Index)
	{
		Durin::FPhysicsBodyDesc Body;
		Body.Geometry = Geometry;
		Body.Transform.Translation = {static_cast<double>(Index % 32) * 2048.0,
			static_cast<double>(Index / 32) * 2048.0, 0.0};
		ASSERT_TRUE(Scene.AddBody(Body).IsValid());
	}
	const auto SceneFacts = Scene.CaptureQueryDiagnostics();
	EXPECT_EQ(SceneFacts.Mutations.UniqueGeometryResources, 1u);
	EXPECT_EQ(SceneFacts.Mutations.RetainedGeometryBytes, Geometry.GetRetainedBytes());

	const auto BuildMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
		BuildEnd - BuildStart).count();
	const auto QueryMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
		QueryEnd - QueryStart).count();
	const auto ReferenceMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
		ReferenceEnd - ReferenceStart).count();
	const auto FirstBodyMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
		FirstBodyEnd - FirstBodyStart).count();
	RecordProperty("heightfield_dimension", Dimension);
	RecordProperty("heightfield_nodes", Facts.NodeCount);
	RecordProperty("heightfield_retained_bytes", Facts.RetainedBytes);
	RecordProperty("heightfield_estimated_peak_bytes", Facts.EstimatedPeakBytes);
	RecordProperty("heightfield_build_microseconds", BuildMicroseconds);
	RecordProperty("heightfield_production_queries", QueryCount);
	RecordProperty("heightfield_query_microseconds", QueryMicroseconds);
	RecordProperty("heightfield_query_cells", Work.HeightFieldCellTests);
	RecordProperty("heightfield_reference_full_scan_microseconds", ReferenceMicroseconds);
	RecordProperty("heightfield_first_body_publish_microseconds", FirstBodyMicroseconds);
}

TEST(FPhysicsSceneAccelerationQualificationTests, MeetsSparseDenseScratchAndRetainedMemoryGatesAtScale)
{
	constexpr size_t BodyCount = 10'000;
	static_assert(Durin::FPhysicsSceneQueryTestAccess::GetBodyRecordSize() == 176);
	static_assert(Durin::FPhysicsSceneQueryTestAccess::GetSlotSize() == 12);
	static_assert(Durin::FPhysicsSceneQueryTestAccess::GetSpatialNodeSize() == 36);
	Durin::FPhysicsScene SparseScene;
	AddDeterministicBodies(SparseScene, BodyCount, EFixtureDistribution::Sparse);
	ASSERT_TRUE(SparseScene.ResetQueryDiagnostics());
	Durin::FPhysicsQueryHit Hit;
	EXPECT_FALSE(SparseScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit));
	Durin::FTransform SweepTransform;
	SweepTransform.Translation = {-10.0, 0.0, 0.0};
	EXPECT_FALSE(SparseScene.SweepSingle(
		Durin::FCollisionShape::MakeCapsule(0.5, 1.0), SweepTransform, {20.0, 0.0, 0.0}, {}, Hit));
	const Durin::FPhysicsSceneQueryDiagnostics Sparse = SparseScene.CaptureQueryDiagnostics();
	EXPECT_LE(GetQueryCounters(Sparse, Durin::EPhysicsSceneQueryKind::LineTraceSingle).Candidates, 100u);
	EXPECT_LE(GetQueryCounters(Sparse, Durin::EPhysicsSceneQueryKind::SweepSingle).Candidates, 100u);
	EXPECT_LE(GetQueryCounters(Sparse, Durin::EPhysicsSceneQueryKind::LineTraceSingle).ScratchHighWater, 128u);
	EXPECT_EQ(Sparse.Mutations.SpatialFallbacks, 0u);
	EXPECT_LE(Sparse.Mutations.RetainedSpatialBytes, 64u * BodyCount + 64u * 1024u);

	Durin::FPhysicsScene DenseScene;
	AddDeterministicBodies(DenseScene, BodyCount, EFixtureDistribution::Dense);
	std::vector<Durin::FPhysicsQueryHit> Hits;
	ASSERT_TRUE(DenseScene.OverlapMulti(
		Durin::FCollisionShape::MakeCapsule(0.5, 1.0), Durin::FTransform(), {}, Hits));
	ASSERT_EQ(Hits.size(), BodyCount);
	EXPECT_TRUE(std::ranges::is_sorted(Hits, {}, &Durin::FPhysicsQueryHit::ActorHandle));
	EXPECT_EQ(DenseScene.CaptureQueryDiagnostics().Mutations.SpatialFallbacks, 0u);
	EXPECT_LE(DenseScene.CaptureQueryDiagnostics().Mutations.RetainedSpatialBytes,
		64u * BodyCount + 64u * 1024u);
}

TEST(FPhysicsSceneAccelerationQualificationTests, RetainedMemoryFitsEveryScaleAndPartitionMix)
{
	for (const size_t BodyCount : FixtureBodyCounts)
	{
		Durin::FPhysicsScene StaticScene;
		Durin::FPhysicsScene MixedScene;
		for (size_t Index = 0; Index < BodyCount; ++Index)
		{
			const Durin::FVector3 Center{100.0 + static_cast<double>(Index % 100) * 4.0,
				-200.0 + static_cast<double>((Index / 100) % 100) * 4.0, 20.0};
			Durin::FPhysicsBodyDesc Static = MakeBoxBody(Center, Index + 1);
			Static.MotionType = Durin::EPhysicsBodyMotionType::Static;
			ASSERT_TRUE(StaticScene.AddBody(Static).IsValid());
			Durin::FPhysicsBodyDesc Mixed = Static;
			if (Index % 2 != 0) Mixed.MotionType = Durin::EPhysicsBodyMotionType::Kinematic;
			ASSERT_TRUE(MixedScene.AddBody(Mixed).IsValid());
		}
		Durin::FPhysicsQueryHit Hit;
		StaticScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		MixedScene.LineTraceSingle({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, Hit);
		const uint64 Budget = 64u * BodyCount + 64u * 1024u;
		EXPECT_LE(StaticScene.CaptureQueryDiagnostics().Mutations.RetainedSpatialBytes, Budget);
		EXPECT_LE(MixedScene.CaptureQueryDiagnostics().Mutations.RetainedSpatialBytes, Budget);
		EXPECT_EQ(StaticScene.CaptureQueryDiagnostics().Mutations.StaticBodies, BodyCount);
		EXPECT_EQ(MixedScene.CaptureQueryDiagnostics().Mutations.StaticBodies, (BodyCount + 1) / 2);
	}
}

TEST(FPhysicsQueryFixtureQualificationTests, DefinesRecordedScalesDistributionsFiltersIgnoresAndChurn)
{
	for (const size_t BodyCount : FixtureBodyCounts)
	{
		SCOPED_TRACE(std::format("seed={}, bodies={}", FixtureSeed, BodyCount));
		Durin::FPhysicsScene Scene;
		std::vector<Durin::FPhysicsActorHandle> Handles = AddDeterministicBodies(
			Scene, BodyCount, EFixtureDistribution::Sparse);
		ASSERT_EQ(Handles.size(), BodyCount);
		EXPECT_EQ(Scene.GetBodyCount(), BodyCount);
		EXPECT_TRUE(std::ranges::all_of(Handles, &Durin::FPhysicsActorHandle::IsValid));
		for (size_t Index = 0; Index < Handles.size(); Index += 3)
		{
			EXPECT_TRUE(Scene.RemoveBody(Handles[Index]));
			EXPECT_FALSE(Scene.RemoveBody(Handles[Index]));
		}
		for (size_t Index = 1; Index < Handles.size(); Index += 3)
		{
			Durin::FPhysicsBodyDesc Updated = MakeBoxBody(
				{500.0 + static_cast<double>(Index), 0.0, 0.0}, static_cast<uint64>(Index + 1));
			Updated.Filter.Responses[0] = Index % 2 == 0
				? Durin::EPhysicsQueryResponse::Ignore
				: Durin::EPhysicsQueryResponse::Overlap;
			EXPECT_TRUE(Scene.UpdateBody(Handles[Index], Updated));
		}
		EXPECT_EQ(Scene.GetBodyCount(), BodyCount - (BodyCount + 2) / 3);
	}
}
