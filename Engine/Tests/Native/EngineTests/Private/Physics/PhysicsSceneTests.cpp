#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "Actors/TerrainActor.h"
#include "Collision/CollisionGeometry.h"
#include "Components/ShapeComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TerrainComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "Modules/ModuleManager.h"
#include "NativeDObjectTestSupport.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsScene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "Terrain/TerrainHeightmap.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>
#include <iostream>
#include <random>

namespace
{
#if defined(_WIN32)
	inline constexpr uint64 ExpectedPrimitiveRetainedBytes =
		DURIN_BUILD_DEBUG ? 208u : 200u;
	inline constexpr uint64 ExpectedCompoundRetainedBytes =
		DURIN_BUILD_DEBUG ? 7'264u : 7'256u;
#else
	inline constexpr uint64 ExpectedPrimitiveRetainedBytes = 200u;
	inline constexpr uint64 ExpectedCompoundRetainedBytes = 7'256u;
#endif
	auto MakeBoxBody(
		const Durin::FVector3& Center,
		const Durin::FVector3& HalfExtent = Durin::FVector3(0.5),
		const Durin::FQuat& Rotation = Durin::FQuatConstants::Identity) -> Durin::FPhysicsBodyDesc
	{
		Durin::FPhysicsBodyDesc Desc;
		Desc.Geometry = Durin::FCollisionGeometryRef::MakePrimitive(
			Durin::FCollisionShape::MakeBox(HalfExtent));
		Desc.Transform = Durin::FTransform();
		Desc.Transform.Translation = Center;
		Desc.Transform.Rotation = Rotation;
		return Desc;
	}

	auto CreatePhysicsWorld() -> Durin::DWorld*
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "PhysicsWorld");
		EXPECT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World, "PhysicsLevel")));
		return World;
	}

	auto AddWorldBox(
		Durin::DWorld& World,
		Durin::FVector3 Center,
		Durin::FVector3 HalfExtent) -> Durin::DBoxComponent*
	{
		auto* Actor = World.SpawnActor<Durin::ACameraActor>("BoxOwner");
		auto* Box = Durin::Cast<Durin::DBoxComponent>(
			Actor->AddInstanceComponent(Durin::DBoxComponent::StaticClass(), "Collision"));
		EXPECT_NE(Box, nullptr);
		EXPECT_TRUE(Box->SetBoxHalfExtent(HalfExtent));
		EXPECT_TRUE(Box->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
		Box->SetWorldLocation(Center);
		return Box;
	}
}

TEST(FPrimitiveComponentCollisionEditingTests, DirectFilterEditsLeaveThePreviousProfile)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	auto* Actor = Durin::NewObject<Durin::ATerrainActor>(nullptr, "CollisionEditingTerrain");
	auto* Component = Actor->GetTerrainComponent();
	ASSERT_NE(Component, nullptr);
	Durin::FProperty* BodyProperty = Component->GetClass()->FindPropertyByName("BodyInstance");
	ASSERT_NE(BodyProperty, nullptr);
	auto* BodyStructProperty = static_cast<Durin::FStructProperty*>(BodyProperty);
	Durin::FProperty* EnabledProperty = BodyStructProperty->GetStruct()->FindPropertyByName(
		Durin::FName("CollisionEnabled"));
	ASSERT_NE(EnabledProperty, nullptr);
	auto* Body = BodyProperty->ContainerPtrToValuePtr<Durin::FBodyInstance>(Component);
	ASSERT_NE(Body, nullptr);
	ASSERT_EQ(Body->ProfileName, Durin::CollisionProfile::NoCollision);

	Body->CollisionEnabled = Durin::ECollisionEnabled::QueryOnly;
	const std::array Path{
		Durin::FPropertyPathSegment{BodyProperty},
		Durin::FPropertyPathSegment{EnabledProperty}
	};
	Component->PostEditChangeProperty({
		BodyProperty, EnabledProperty, Path, Durin::EPropertyChangePhase::Committed,
		Durin::EPropertyChangeKind::ValueSet, Durin::EPropertyChangeOrigin::Edit
	});

	EXPECT_EQ(Component->GetCollisionEnabled(), Durin::ECollisionEnabled::QueryOnly);
	EXPECT_TRUE(Component->GetCollisionProfileName().IsNone());
	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FPhysicsPublicContractTests, FreezesCompleteNamesAndReflectionIdentities)
{
	static_assert(std::same_as<decltype(std::declval<Durin::DWorld&>().GetPhysicsScene()), Durin::FPhysicsScene&>);
	static_assert(requires(Durin::DWorld& World, Durin::FHitResult& Hit) {
		World.LineTraceSingleByChannel(Hit, Durin::FVector3(0.0), Durin::FVector3(1.0), Durin::ECollisionChannel::Visibility);
	});
	Durin::Testing::InitializeDObjectSystemForTests();
	EXPECT_EQ(Durin::DBodySetup::StaticClass()->GetQualifiedName().ToString(), "Durin::DBodySetup");
	EXPECT_EQ(Durin::DShapeComponent::StaticClass()->GetQualifiedName().ToString(), "Durin::DShapeComponent");
	EXPECT_EQ(Durin::DBoxComponent::StaticClass()->GetQualifiedName().ToString(), "Durin::DBoxComponent");
	EXPECT_EQ(Durin::DCapsuleComponent::StaticClass()->GetQualifiedName().ToString(), "Durin::DCapsuleComponent");
	EXPECT_NE(Durin::DPrimitiveComponent::StaticClass()->FindPropertyByName("BodyInstance"), nullptr);
	EXPECT_NE(Durin::DCapsuleComponent::StaticClass()->FindPropertyByName("CapsuleRadius"), nullptr);
	EXPECT_NE(Durin::DCapsuleComponent::StaticClass()->FindPropertyByName("CapsuleHalfHeight"), nullptr);
	Durin::FProperty* Dimensions = Durin::DBodySetup::StaticClass()->FindPropertyByName("Dimensions");
	ASSERT_NE(Dimensions, nullptr);
	EXPECT_EQ(Dimensions->GetTypedMetadata().Category, "Shape");
	EXPECT_EQ(Dimensions->GetTypedMetadata().Units, Durin::EPropertyUnit::Meters);
}

TEST(FPhysicsGeometryTests, RaycastsRotatedPositiveScaleBoxes)
{
	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc Body = MakeBoxBody(
		{0.0, 0.0, 0.0}, {1.0, 0.5, 0.5},
		Durin::Math::MakeQuaternionFromAxisAngleDegrees(45.0, Durin::FVectorConstants::Up));
	Body.Transform.Scale3D = {2.0, 1.0, 1.0};
	ASSERT_TRUE(Scene.AddBody(Body).IsValid());
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-4.0, 0.0, 0.0}, {4.0, 0.0, 0.0}, {}, Hit));
	EXPECT_GT(Hit.Time, 0.0);
	EXPECT_LT(Hit.Time, 0.5);
	EXPECT_NEAR(Durin::Math::Length(Hit.ImpactNormal), 1.0, 1.e-8);
}

TEST(FPhysicsGeometryTests, SweepsCapsulesAndReportsInitialPenetration)
{
	Durin::FPhysicsScene Scene;
	ASSERT_TRUE(Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}, {0.5, 3.0, 3.0})).IsValid());
	Durin::FTransform CapsuleTransform;
	CapsuleTransform.Translation = {-3.0, 0.0, 0.0};
	const Durin::FCollisionShape Capsule = Durin::FCollisionShape::MakeCapsule(0.4, 1.0);
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.SweepSingle(Capsule, CapsuleTransform, {6.0, 0.0, 0.0}, {}, Hit));
	EXPECT_NEAR(Hit.Time, 0.35, 2.e-3);
	EXPECT_LT(Hit.ImpactNormal.x, -0.99);
	CapsuleTransform.Translation = {0.0, 0.0, 0.0};
	ASSERT_TRUE(Scene.SweepSingle(Capsule, CapsuleTransform, {1.0, 0.0, 0.0}, {}, Hit));
	EXPECT_TRUE(Hit.bStartPenetrating);
	EXPECT_DOUBLE_EQ(Hit.Time, 0.0);
	EXPECT_GE(Hit.PenetrationDepth, 0.0);
}

TEST(FPhysicsHeightFieldTests, BuildsExactRegularGridWithoutExpandedTriangles)
{
	const std::array<uint16, 4> Samples{0, 0, 0, 65535};
	Durin::FCollisionGeometryBuildDiagnostics Diagnostics;
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::BuildHeightField(
		2, 2, Samples, 1.0, 1.0, 10.0, 0.0, &Diagnostics);
	ASSERT_TRUE(Geometry.IsValid());
	EXPECT_EQ(Geometry.GetKind(), Durin::ECollisionGeometryKind::HeightField);
	EXPECT_EQ(Diagnostics.Status, Durin::ECollisionGeometryBuildStatus::Success);
	EXPECT_EQ(Geometry.GetVertexCount(), 0u);
	EXPECT_EQ(Geometry.GetTriangleCount(), 2u);
	EXPECT_EQ(Geometry.GetLeafTriangleCount(), 0u);
	EXPECT_EQ(Geometry.GetHeightFieldRegionCount(), 1u);
	Durin::FVector3 Minimum;
	Durin::FVector3 Maximum;
	ASSERT_TRUE(Geometry.GetLocalBounds(Minimum, Maximum));
	EXPECT_EQ(Minimum, Durin::FVector3(0.0));
	EXPECT_EQ(Maximum, Durin::FVector3(1.0, 1.0, 10.0));

	Durin::FTransform Transform;
	for (const auto& Fixture : std::array{
		std::pair{Durin::FVector3(0.25, 0.25, 0.0), 0.5},
		std::pair{Durin::FVector3(0.75, 0.75, 0.0), 0.375}})
	{
		Durin::FPhysicsQueryHit Reference;
		Durin::FPhysicsQueryHit Production;
		const Durin::FVector3 Start(Fixture.first.x, Fixture.first.y, 20.0);
		const Durin::FVector3 End(Fixture.first.x, Fixture.first.y, -20.0);
		EXPECT_EQ(Durin::CollisionGeometry::Raycast(Start, End, Geometry, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_EQ(Durin::CollisionGeometry::Raycast(Start, End, Geometry, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_NEAR(Reference.Time, Fixture.second, 1.0e-12);
		EXPECT_NEAR(Production.Time, Reference.Time, 1.0e-12);
		EXPECT_NEAR(Production.ImpactPoint.z, Reference.ImpactPoint.z, 1.0e-8);
	}

	for (const Durin::FCollisionShape Shape : {
		Durin::FCollisionShape::MakeSphere(0.5),
		Durin::FCollisionShape::MakeCapsule(0.25, 0.5),
		Durin::FCollisionShape::MakeBox({0.25, 0.25, 0.25})})
	{
		SCOPED_TRACE(static_cast<int>(Shape.GetType()));
		Durin::FTransform QueryTransform;
		QueryTransform.Translation = {0.25, 0.25, 2.0};
		Durin::FPhysicsQueryHit Reference;
		Durin::FPhysicsQueryHit Production;
		EXPECT_EQ(Durin::CollisionGeometry::Sweep(Shape, QueryTransform, {0.0, 0.0, -4.0},
			Geometry, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_EQ(Durin::CollisionGeometry::Sweep(Shape, QueryTransform, {0.0, 0.0, -4.0},
			Geometry, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_NEAR(Production.Time, Reference.Time, 1.0e-10);
		QueryTransform.Translation.z = 0.1;
		EXPECT_EQ(Durin::CollisionGeometry::Overlap(Shape, QueryTransform, Geometry, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_EQ(Durin::CollisionGeometry::Overlap(Shape, QueryTransform, Geometry, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_NEAR(Production.PenetrationDepth, Reference.PenetrationDepth, 1.0e-8);
	}
}

TEST(FPhysicsHeightFieldTests, MatchesExplicitMeshOracleAcrossStructuralAndContactFixtures)
{
	constexpr uint32 Width = 3;
	constexpr uint32 Height = 3;
	const std::array<std::array<uint16, Width * Height>, 5> Fixtures{{
		{0, 0, 0, 0, 0, 0, 0, 0, 0},
		{0, 65535, 0, 65535, 0, 65535, 0, 65535, 0},
		{65535, 0, 65535, 0, 65535, 0, 65535, 0, 65535},
		{0, 0, 65535, 0, 32768, 65535, 65535, 65535, 65535},
		{65535, 65535, 0, 65535, 32768, 0, 0, 0, 0}}};
	const std::array<Durin::FVector3, 9> QueryPoints{
		Durin::FVector3(0.0, 0.0, 0.0), Durin::FVector3(2.0, 2.0, 0.0),
		Durin::FVector3(1.0, 1.0, 0.0), Durin::FVector3(1.0, 0.5, 0.0),
		Durin::FVector3(0.5, 1.0, 0.0), Durin::FVector3(0.5, 0.5, 0.0),
		Durin::FVector3(1.5, 1.5, 0.0), Durin::FVector3(0.000001, 1.999999, 0.0),
		Durin::FVector3(1.999999, 0.000001, 0.0)};
	auto ExpectHitParity = [](Durin::CollisionGeometry::ECollisionQueryStatus ExpectedStatus,
		Durin::CollisionGeometry::ECollisionQueryStatus ActualStatus,
		const Durin::FPhysicsQueryHit& Expected, const Durin::FPhysicsQueryHit& Actual) {
		ASSERT_EQ(ActualStatus, ExpectedStatus);
		if (ExpectedStatus != Durin::CollisionGeometry::ECollisionQueryStatus::Hit) return;
		EXPECT_NEAR(Actual.Time, Expected.Time, 1.0e-9);
		EXPECT_NEAR(Actual.ImpactPoint.x, Expected.ImpactPoint.x, 1.0e-8);
		EXPECT_NEAR(Actual.ImpactPoint.y, Expected.ImpactPoint.y, 1.0e-8);
		EXPECT_NEAR(Actual.ImpactPoint.z, Expected.ImpactPoint.z, 1.0e-8);
		EXPECT_NEAR(Actual.ImpactNormal.x, Expected.ImpactNormal.x, 1.0e-8);
		EXPECT_NEAR(Actual.ImpactNormal.y, Expected.ImpactNormal.y, 1.0e-8);
		EXPECT_NEAR(Actual.ImpactNormal.z, Expected.ImpactNormal.z, 1.0e-8);
		EXPECT_NEAR(Actual.PenetrationDepth, Expected.PenetrationDepth, 1.0e-8);
		EXPECT_EQ(Actual.bStartPenetrating, Expected.bStartPenetrating);
	};

	for (size_t FixtureIndex = 0; FixtureIndex < Fixtures.size(); ++FixtureIndex)
	{
		SCOPED_TRACE(std::format("fixture={}", FixtureIndex));
		const double HeightScale = FixtureIndex == Fixtures.size() - 1 ? -12.0 : 12.0;
		const double HeightOffset = FixtureIndex == Fixtures.size() - 1 ? 6.0 : -6.0;
		const auto& Samples = Fixtures[FixtureIndex];
		Durin::FCollisionGeometryBuildDiagnostics Facts;
		const Durin::FCollisionGeometryRef HeightField = Durin::FCollisionGeometryRef::BuildHeightField(
			Width, Height, Samples, 1.0, 1.0, HeightScale, HeightOffset, &Facts);
		ASSERT_TRUE(HeightField.IsValid());
		const Durin::FCollisionGeometryRef Shared = Durin::FCollisionGeometryRef::BuildHeightField(
			Width, Height, Samples, 1.0, 1.0, HeightScale, HeightOffset);
		ASSERT_TRUE(Shared.IsValid());
		EXPECT_EQ(Shared.GetIdentity(), HeightField.GetIdentity());
		EXPECT_EQ(HeightField.GetRetainedBytes(), Facts.RetainedBytes);
		const Durin::FCollisionGeometryRef DifferentInterpretation =
			Durin::FCollisionGeometryRef::BuildHeightField(
				Width, Height, Samples, 1.0, 1.0, HeightScale, HeightOffset + 1.0);
		ASSERT_TRUE(DifferentInterpretation.IsValid());
		EXPECT_NE(DifferentInterpretation.GetIdentity(), HeightField.GetIdentity());

		std::vector<Durin::FVector3> Vertices;
		std::vector<uint32> Indices;
		Vertices.reserve(Width * Height);
		Indices.reserve((Width - 1) * (Height - 1) * 6);
		for (uint32 Y = 0; Y < Height; ++Y)
			for (uint32 X = 0; X < Width; ++X)
				Vertices.emplace_back(X, Y, HeightOffset
					+ static_cast<double>(Samples[Y * Width + X]) / 65535.0 * HeightScale);
		for (uint32 Y = 0; Y + 1 < Height; ++Y)
			for (uint32 X = 0; X + 1 < Width; ++X)
			{
				const uint32 A = Y * Width + X;
				const uint32 B = A + 1;
				const uint32 C = A + Width;
				const uint32 D = C + 1;
				Indices.insert(Indices.end(), {A, B, C, B, D, C});
			}
		const Durin::FCollisionGeometryRef Oracle =
			Durin::FCollisionGeometryRef::BuildTriangleMesh(Vertices, Indices);
		ASSERT_TRUE(Oracle.IsValid());
		Durin::FTransform Transform;
		Transform.Translation = {8.0, -3.0, 2.0};
		Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
			17.0, Durin::FVectorConstants::Up);
		Transform.Scale3D = {1.25, 0.75, 1.5};
		auto ToWorld = [&](const Durin::FVector3& Local) {
			return Transform.Translation + Durin::Math::RotateVector(
				Transform.Rotation, Transform.Scale3D * Local);
		};

		for (size_t PointIndex = 0; PointIndex < QueryPoints.size(); ++PointIndex)
		{
			SCOPED_TRACE(std::format("point={}", PointIndex));
			const Durin::FVector3 Local = QueryPoints[PointIndex];
			Durin::FPhysicsQueryHit OracleHit;
			Durin::FPhysicsQueryHit HeightFieldHit;
			const Durin::FVector3 Start = ToWorld(Local + Durin::FVector3(0.0, 0.0, 20.0));
			const Durin::FVector3 End = ToWorld(Local + Durin::FVector3(0.0, 0.0, -20.0));
			const auto OracleRay = Durin::CollisionGeometry::Raycast(Start, End, Oracle, Transform,
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, OracleHit);
			const auto HeightFieldRay = Durin::CollisionGeometry::Raycast(Start, End, HeightField, Transform,
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, HeightFieldHit);
			{
				SCOPED_TRACE("ray");
				ExpectHitParity(OracleRay, HeightFieldRay, OracleHit, HeightFieldHit);
			}
			ASSERT_EQ(HeightFieldRay, Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			const Durin::FVector3 SurfacePoint = HeightFieldHit.ImpactPoint;
			if (FixtureIndex != 0) continue;

			const Durin::FCollisionShape Shape = PointIndex % 3 == 0
				? Durin::FCollisionShape::MakeSphere(0.2)
				: PointIndex % 3 == 1
					? Durin::FCollisionShape::MakeCapsule(0.15, 0.3)
					: Durin::FCollisionShape::MakeBox({0.15, 0.1, 0.2});
			Durin::FTransform QueryTransform;
			QueryTransform.Translation = Start;
			const Durin::FVector3 Delta = End - Start;
			const auto OracleSweep = Durin::CollisionGeometry::Sweep(Shape, QueryTransform, Delta,
				Oracle, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, OracleHit);
			const auto HeightFieldSweep = Durin::CollisionGeometry::Sweep(Shape, QueryTransform, Delta,
				HeightField, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, HeightFieldHit);
			{
				SCOPED_TRACE("sweep");
				ExpectHitParity(OracleSweep, HeightFieldSweep, OracleHit, HeightFieldHit);
			}
			ASSERT_EQ(HeightFieldSweep, Durin::CollisionGeometry::ECollisionQueryStatus::Hit);

			QueryTransform.Translation = SurfacePoint;
			const auto OracleOverlap = Durin::CollisionGeometry::Overlap(Shape, QueryTransform,
				Oracle, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, OracleHit);
			const auto HeightFieldOverlap = Durin::CollisionGeometry::Overlap(Shape, QueryTransform,
				HeightField, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, HeightFieldHit);
			{
				SCOPED_TRACE("overlap");
				ExpectHitParity(OracleOverlap, HeightFieldOverlap, OracleHit, HeightFieldHit);
			}
		}
	}
}

TEST(FPhysicsHeightFieldTests, BoundsProductionWorkAtFrozenMaximum)
{
	constexpr uint32 Dimension = 1025;
	std::vector<uint16> Samples(static_cast<size_t>(Dimension) * Dimension, 32768);
	Durin::FCollisionGeometryBuildDiagnostics Diagnostics;
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::BuildHeightField(
		Dimension, Dimension, Samples, 1.0, 1.0, 100.0, -50.0, &Diagnostics);
	ASSERT_TRUE(Geometry.IsValid());
	EXPECT_EQ(Diagnostics.NodeCount, 32767u);
	EXPECT_EQ(Diagnostics.MaximumDepth, 15u);
	EXPECT_LT(Diagnostics.RetainedBytes, 4u * 1024u * 1024u);
	EXPECT_LT(Diagnostics.EstimatedPeakBytes, 7u * 1024u * 1024u);
	Durin::CollisionGeometry::FCollisionGeometryCounters Counters;
	Durin::FPhysicsQueryHit Hit;
	EXPECT_EQ(Durin::CollisionGeometry::Raycast({512.5, 512.5, 10.0},
		{512.5, 512.5, -10.0}, Geometry, {},
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit, &Counters),
		Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	EXPECT_LE(Counters.AssetNodeTests, 32u);
	EXPECT_LE(Counters.FeatureTests, 128u);
	EXPECT_GT(Counters.HeightFieldCellTests, 0u);
	EXPECT_EQ(Counters.HeightFieldTriangleTests, Counters.HeightFieldCellTests * 2u);
	EXPECT_LT(Counters.HeightFieldCellTests,
		static_cast<uint64>(Dimension - 1) * (Dimension - 1));
	EXPECT_EQ(Counters.ReferenceFallbacks, 0u);
	EXPECT_EQ(Counters.Unsupported, 0u);
	EXPECT_EQ(Counters.NonConverged, 0u);
	EXPECT_FALSE(Counters.bOverflowed);
	Durin::CollisionGeometry::FCollisionGeometryCounters Saturated;
	Saturated.HeightFieldCellTests = std::numeric_limits<uint64>::max();
	Saturated.HeightFieldTriangleTests = std::numeric_limits<uint64>::max();
	EXPECT_EQ(Durin::CollisionGeometry::Raycast({512.5, 512.5, 10.0},
		{512.5, 512.5, -10.0}, Geometry, {},
		Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit, &Saturated),
		Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	EXPECT_EQ(Saturated.HeightFieldCellTests, std::numeric_limits<uint64>::max());
	EXPECT_EQ(Saturated.HeightFieldTriangleTests, std::numeric_limits<uint64>::max());
	EXPECT_TRUE(Saturated.bOverflowed);

	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc Body;
	Body.Geometry = Geometry;
	ASSERT_TRUE(Scene.AddBody(Body).IsValid());
	ASSERT_TRUE(Scene.SetQueryExecutionPolicy(Durin::EPhysicsSceneQueryExecutionPolicy::Production));
	ASSERT_TRUE(Scene.ResetQueryDiagnostics());
	ASSERT_TRUE(Scene.LineTraceSingle({512.5, 512.5, 10.0},
		{512.5, 512.5, -10.0}, {}, Hit));
	const auto SceneDiagnostics = Scene.CaptureQueryDiagnostics();
	const auto& SceneCounters = SceneDiagnostics.Queries[
		static_cast<size_t>(Durin::EPhysicsSceneQueryKind::LineTraceSingle)];
	EXPECT_EQ(SceneCounters.HeightFieldTriangleTests,
		SceneCounters.HeightFieldCellTests * 2u);
	EXPECT_GT(SceneCounters.HeightFieldCellTests, 0u);
}

TEST(FPhysicsHeightFieldTests, QualifiesTangencyPenetrationZeroLengthAndUpwardMotion)
{
	const std::array<uint16, 4> Samples{0, 0, 0, 0};
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::BuildHeightField(
		2, 2, Samples, 1.0, 1.0, 1.0, 0.0);
	ASSERT_TRUE(Geometry.IsValid());
	auto ExpectParity = [](Durin::CollisionGeometry::ECollisionQueryStatus ReferenceStatus,
		Durin::CollisionGeometry::ECollisionQueryStatus ProductionStatus,
		const Durin::FPhysicsQueryHit& Reference, const Durin::FPhysicsQueryHit& Production) {
		ASSERT_EQ(ProductionStatus, ReferenceStatus);
		if (ReferenceStatus != Durin::CollisionGeometry::ECollisionQueryStatus::Hit) return;
		EXPECT_NEAR(Production.Time, Reference.Time, 1.0e-10);
		EXPECT_NEAR(Production.PenetrationDepth, Reference.PenetrationDepth, 1.0e-10);
		EXPECT_EQ(Production.bStartPenetrating, Reference.bStartPenetrating);
	};
	Durin::FPhysicsQueryHit Reference;
	Durin::FPhysicsQueryHit Production;
	EXPECT_EQ(Durin::CollisionGeometry::Raycast({0.5, 0.5, 0.0}, {0.5, 0.5, 0.0},
		Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
		Durin::CollisionGeometry::Raycast({0.5, 0.5, 0.0}, {0.5, 0.5, 0.0},
			Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production));

	const std::array<std::pair<Durin::FCollisionShape, double>, 3> Shapes{
		std::pair{Durin::FCollisionShape::MakeSphere(0.5), 0.5},
		std::pair{Durin::FCollisionShape::MakeCapsule(0.25, 0.75), 0.75},
		std::pair{Durin::FCollisionShape::MakeBox({0.25, 0.25, 0.5}), 0.5}};
	for (const auto& [Shape, VerticalExtent] : Shapes)
	{
		SCOPED_TRACE(static_cast<int>(Shape.GetType()));
		Durin::FTransform QueryTransform;
		QueryTransform.Translation = {0.5, 0.5, VerticalExtent};
		auto ReferenceStatus = Durin::CollisionGeometry::Overlap(Shape, QueryTransform,
			Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference);
		auto ProductionStatus = Durin::CollisionGeometry::Overlap(Shape, QueryTransform,
			Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production);
		ExpectParity(ReferenceStatus, ProductionStatus, Reference, Production);

		QueryTransform.Translation = {0.5, 0.5, 0.0};
		ReferenceStatus = Durin::CollisionGeometry::Overlap(Shape, QueryTransform,
			Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference);
		ProductionStatus = Durin::CollisionGeometry::Overlap(Shape, QueryTransform,
			Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production);
		ExpectParity(ReferenceStatus, ProductionStatus, Reference, Production);
		ASSERT_EQ(ProductionStatus, Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_GT(Production.PenetrationDepth, 0.0);

		QueryTransform.Translation = {0.5, 0.5, -2.0 - VerticalExtent};
		ReferenceStatus = Durin::CollisionGeometry::Sweep(Shape, QueryTransform, {0.0, 0.0, 4.0},
			Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference);
		Durin::CollisionGeometry::FCollisionGeometryCounters Work;
		ProductionStatus = Durin::CollisionGeometry::Sweep(Shape, QueryTransform, {0.0, 0.0, 4.0},
			Geometry, {}, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production, &Work);
		ExpectParity(ReferenceStatus, ProductionStatus, Reference, Production);
		EXPECT_EQ(Work.ReferenceFallbacks, 0u);
		EXPECT_EQ(Work.Unsupported, 0u);
		EXPECT_EQ(Work.NonConverged, 0u);
		EXPECT_FALSE(Work.bOverflowed);
	}
}

TEST(FPhysicsHeightFieldTests, MatchesReferenceAcrossGoldenBoundariesAndFixedSeedQueries)
{
	constexpr uint32 Width = 9;
	constexpr uint32 Height = 7;
	std::array<uint16, Width * Height> Samples{};
	for (uint32 Y = 0; Y < Height; ++Y)
		for (uint32 X = 0; X < Width; ++X)
			Samples[Y * Width + X] = static_cast<uint16>(
				(X * 8191u + Y * 12731u + ((X + Y) % 3u) * 4096u) & 0xffffu);
	Samples[0] = 0;
	Samples[Width * Height - 1] = 65535;
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::BuildHeightField(
		Width, Height, Samples, 1.25, 0.75, -18.0, 6.0);
	ASSERT_TRUE(Geometry.IsValid());
	Durin::FTransform TargetTransform;
	TargetTransform.Translation = {13.0, -7.0, 4.0};
	TargetTransform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		23.0, Durin::FVectorConstants::Up);
	TargetTransform.Scale3D = {1.5, 0.8, 1.25};
	auto ToWorld = [&](const Durin::FVector3& Local) {
		return TargetTransform.Translation + Durin::Math::RotateVector(
			TargetTransform.Rotation, TargetTransform.Scale3D * Local);
	};
	auto ExpectParity = [](Durin::CollisionGeometry::ECollisionQueryStatus ReferenceStatus,
		Durin::CollisionGeometry::ECollisionQueryStatus ProductionStatus,
		const Durin::FPhysicsQueryHit& Reference, const Durin::FPhysicsQueryHit& Production) {
		ASSERT_EQ(ProductionStatus, ReferenceStatus);
		if (ReferenceStatus != Durin::CollisionGeometry::ECollisionQueryStatus::Hit) return;
		EXPECT_NEAR(Production.Time, Reference.Time, 1.0e-9);
		EXPECT_NEAR(Production.ImpactPoint.x, Reference.ImpactPoint.x, 1.0e-7);
		EXPECT_NEAR(Production.ImpactPoint.y, Reference.ImpactPoint.y, 1.0e-7);
		EXPECT_NEAR(Production.ImpactPoint.z, Reference.ImpactPoint.z, 1.0e-7);
		EXPECT_NEAR(Production.ImpactNormal.x, Reference.ImpactNormal.x, 1.0e-7);
		EXPECT_NEAR(Production.ImpactNormal.y, Reference.ImpactNormal.y, 1.0e-7);
		EXPECT_NEAR(Production.ImpactNormal.z, Reference.ImpactNormal.z, 1.0e-7);
		EXPECT_NEAR(Production.PenetrationDepth, Reference.PenetrationDepth, 1.0e-7);
		EXPECT_EQ(Production.bStartPenetrating, Reference.bStartPenetrating);
	};

	std::mt19937_64 Generator(0x4846'5041'5249'5459ull);
	std::uniform_real_distribution<double> Unit(0.0, 1.0);
	const std::array<Durin::FVector3, 8> GoldenLocalPoints{
		Durin::FVector3(0.0, 0.0, 0.0), Durin::FVector3(10.0, 4.5, 0.0),
		Durin::FVector3(1.25, 0.75, 0.0), Durin::FVector3(2.5, 1.5, 0.0),
		Durin::FVector3(0.625, 0.375, 0.0), Durin::FVector3(4.375, 2.625, 0.0),
		Durin::FVector3(9.999999, 0.000001, 0.0), Durin::FVector3(5.0, 4.499999, 0.0)};
	for (size_t Index = 0; Index < 72; ++Index)
	{
		const Durin::FVector3 Local = Index < GoldenLocalPoints.size()
			? GoldenLocalPoints[Index]
			: Durin::FVector3(Unit(Generator) * 10.0, Unit(Generator) * 4.5, 0.0);
		SCOPED_TRACE(std::format("query={}", Index));
		Durin::FPhysicsQueryHit Reference;
		Durin::FPhysicsQueryHit Production;
		const auto ReferenceRay = Durin::CollisionGeometry::Raycast(
			ToWorld(Local + Durin::FVector3(0.0, 0.0, 30.0)),
			ToWorld(Local + Durin::FVector3(0.0, 0.0, -30.0)), Geometry, TargetTransform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference);
		Durin::CollisionGeometry::FCollisionGeometryCounters Counters;
		const auto ProductionRay = Durin::CollisionGeometry::Raycast(
			ToWorld(Local + Durin::FVector3(0.0, 0.0, 30.0)),
			ToWorld(Local + Durin::FVector3(0.0, 0.0, -30.0)), Geometry, TargetTransform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production, &Counters);
		ExpectParity(ReferenceRay, ProductionRay, Reference, Production);
		EXPECT_EQ(Counters.HeightFieldTriangleTests, Counters.HeightFieldCellTests * 2u);

		const Durin::FCollisionShape Shape = Index % 3 == 0
			? Durin::FCollisionShape::MakeSphere(0.35)
			: Index % 3 == 1
				? Durin::FCollisionShape::MakeCapsule(0.25, 0.6)
				: Durin::FCollisionShape::MakeBox({0.3, 0.2, 0.25});
		Durin::FTransform QueryTransform;
		QueryTransform.Translation = ToWorld(Local + Durin::FVector3(0.0, 0.0,
			-4.0 + Unit(Generator) * 18.0));
		QueryTransform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
			Unit(Generator) * 90.0, Durin::FVectorConstants::Up);
		const Durin::FVector3 Delta = ToWorld(Local + Durin::FVector3(0.0, 0.0, -20.0))
			- ToWorld(Local);
		const auto ReferenceSweep = Durin::CollisionGeometry::Sweep(
			Shape, QueryTransform, Delta, Geometry, TargetTransform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference);
		const auto ProductionSweep = Durin::CollisionGeometry::Sweep(
			Shape, QueryTransform, Delta, Geometry, TargetTransform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production);
		ExpectParity(ReferenceSweep, ProductionSweep, Reference, Production);
		const auto ReferenceOverlap = Durin::CollisionGeometry::Overlap(
			Shape, QueryTransform, Geometry, TargetTransform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference);
		const auto ProductionOverlap = Durin::CollisionGeometry::Overlap(
			Shape, QueryTransform, Geometry, TargetTransform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production);
		ExpectParity(ReferenceOverlap, ProductionOverlap, Reference, Production);
	}
}

TEST(FPhysicsHeightFieldTests, RejectsInvalidExtentsAndSupportsSignedHeightTransforms)
{
	const std::array<uint16, 4> Samples{0, 0, 0, 65535};
	Durin::FCollisionGeometryBuildDiagnostics Diagnostics;
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildHeightField(
		1, 2, std::span(Samples).first(2), 1.0, 1.0, 1.0, 0.0, &Diagnostics));
	EXPECT_EQ(Diagnostics.Status, Durin::ECollisionGeometryBuildStatus::InvalidInput);
	std::vector<uint16> Oversized(1026u * 2u, 0);
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildHeightField(
		1026, 2, Oversized, 1.0, 1.0, 1.0, 0.0, &Diagnostics));
	EXPECT_EQ(Diagnostics.Status, Durin::ECollisionGeometryBuildStatus::LimitExceeded);
	const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::BuildHeightField(
		2, 2, Samples, 1.0, 1.0, -10.0, 0.0, &Diagnostics);
	ASSERT_TRUE(Geometry.IsValid());
	Durin::FVector3 Minimum;
	Durin::FVector3 Maximum;
	ASSERT_TRUE(Geometry.GetLocalBounds(Minimum, Maximum));
	EXPECT_DOUBLE_EQ(Minimum.z, -10.0);
	EXPECT_DOUBLE_EQ(Maximum.z, 0.0);
	Durin::FTransform Transform;
	Transform.Translation = {10.0, -2.0, 3.0};
	Transform.Scale3D = {2.0, 3.0, 1.0};
	Durin::FPhysicsQueryHit Reference;
	Durin::FPhysicsQueryHit Production;
	EXPECT_EQ(Durin::CollisionGeometry::Raycast({11.5, 0.25, 10.0}, {11.5, 0.25, -10.0},
		Geometry, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
		Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	EXPECT_EQ(Durin::CollisionGeometry::Raycast({11.5, 0.25, 10.0}, {11.5, 0.25, -10.0},
		Geometry, Transform, Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production),
		Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
	EXPECT_NEAR(Production.Time, Reference.Time, 1.0e-12);
	EXPECT_NEAR(Production.ImpactPoint.z, -2.0, 1.0e-8);
}

TEST(FPhysicsTerrainTests, PublishesSharedRevisionThroughOrdinaryWorldQueries)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	const std::array<uint16, 4> Samples{0, 0, 0, 65535};
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(World, "CollisionHeightmap");
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, Samples, Error)) << Error;
	auto* FirstActor = World->SpawnActor<Durin::ATerrainActor>("FirstTerrain");
	auto* SecondActor = World->SpawnActor<Durin::ATerrainActor>("SecondTerrain");
	ASSERT_NE(FirstActor, nullptr);
	ASSERT_NE(SecondActor, nullptr);
	Durin::DTerrainComponent* First = FirstActor->GetTerrainComponent();
	Durin::DTerrainComponent* Second = SecondActor->GetTerrainComponent();
	ASSERT_TRUE(First->SetSampleSpacing(1.0, 1.0));
	ASSERT_TRUE(Second->SetSampleSpacing(1.0, 1.0));
	ASSERT_TRUE(First->SetHeightRange(10.0, 0.0));
	ASSERT_TRUE(Second->SetHeightRange(10.0, 0.0));
	First->SetHeightmap(Heightmap);
	Second->SetHeightmap(Heightmap);
	ASSERT_TRUE(First->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
	ASSERT_TRUE(Second->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
	ASSERT_TRUE(First->RequestPhysicsStateCreation(true));
	ASSERT_TRUE(Second->RequestPhysicsStateCreation(true));
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 2u);
	EXPECT_EQ(First->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Ready);
	EXPECT_EQ(Second->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Ready);
	Durin::FCollisionGeometryRef FirstGeometry;
	Durin::FCollisionGeometryRef SecondGeometry;
	Durin::FTransform FirstTransform;
	Durin::FTransform SecondTransform;
	ASSERT_TRUE(First->BuildCollisionGeometry(FirstGeometry, FirstTransform));
	ASSERT_TRUE(Second->BuildCollisionGeometry(SecondGeometry, SecondTransform));
	EXPECT_EQ(FirstGeometry.GetIdentity(), SecondGeometry.GetIdentity());
	const Durin::FTerrainCollisionFacts CollisionFacts = First->GetCollisionFacts();
	EXPECT_EQ(CollisionFacts.Status, Durin::ETerrainCollisionStatus::Ready);
	EXPECT_EQ(CollisionFacts.AssetRevision, Heightmap->GetRevision());
	EXPECT_GT(CollisionFacts.CollisionRevision, 0u);
	EXPECT_EQ(CollisionFacts.ResourceIdentity, FirstGeometry.GetIdentity());
	EXPECT_EQ(CollisionFacts.RetainedBytes, FirstGeometry.GetRetainedBytes());
	EXPECT_EQ(CollisionFacts.Width, 2u);
	EXPECT_EQ(CollisionFacts.Height, 2u);
	EXPECT_EQ(CollisionFacts.Cells, 1u);
	EXPECT_EQ(CollisionFacts.Nodes, 1u);
	EXPECT_EQ(CollisionFacts.MaximumDepth, 1u);
	EXPECT_EQ(CollisionFacts.BuildStatus, Durin::ECollisionGeometryBuildStatus::Success);
	Durin::FHitResult Hit;
	ASSERT_TRUE(World->LineTraceSingleByChannel(Hit, {0.75, 0.75, 20.0},
		{0.75, 0.75, -20.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_NEAR(Hit.ImpactPoint.z, 5.0, 1.0e-8);
	EXPECT_TRUE(Hit.Component == First || Hit.Component == Second);
	const Durin::FCollisionShape QuerySphere = Durin::FCollisionShape::MakeSphere(0.25);
	Durin::FTransform QueryTransform;
	QueryTransform.Translation = {0.75, 0.75, 20.0};
	ASSERT_TRUE(World->SweepSingleByChannel(Hit, QuerySphere, QueryTransform,
		{0.0, 0.0, -30.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_TRUE(Hit.Component == First || Hit.Component == Second);
	QueryTransform.Translation = {0.75, 0.75, 5.0};
	std::vector<Durin::FOverlapResult> Overlaps;
	ASSERT_TRUE(World->OverlapMultiByChannel(Overlaps, QuerySphere, QueryTransform,
		Durin::ECollisionChannel::Visibility));
	EXPECT_EQ(Overlaps.size(), 2u);
	Durin::FCollisionQueryParams IgnoreFirst;
	IgnoreFirst.AddIgnoredComponent(First);
	ASSERT_TRUE(World->OverlapMultiByChannel(Overlaps, QuerySphere, QueryTransform,
		Durin::ECollisionChannel::Visibility, IgnoreFirst));
	ASSERT_EQ(Overlaps.size(), 1u);
	EXPECT_EQ(Overlaps[0].Component, Second);
	World->SetCollisionDebugDrawEnabled(true);
	const Durin::FCollisionDebugSnapshot Debug = World->CaptureCollisionDebugSnapshot();
	ASSERT_EQ(Debug.Bodies.size(), 2u);
	EXPECT_EQ(Debug.Bodies[0].GeometryKind, Durin::ECollisionGeometryKind::HeightField);
	EXPECT_EQ(Debug.Bodies[0].ResourceIdentity, FirstGeometry.GetIdentity());
	EXPECT_EQ(Debug.Bodies[0].RetainedBytes, FirstGeometry.GetRetainedBytes());
	EXPECT_EQ(Debug.Bodies[0].TotalTriangles, 2u);
	EXPECT_EQ(Debug.Bodies[0].HeightFieldWidth, 2u);
	EXPECT_EQ(Debug.Bodies[0].HeightFieldHeight, 2u);
	EXPECT_EQ(Debug.Bodies[0].HeightFieldNodes, 1u);
	EXPECT_EQ(Debug.Bodies[0].HeightFieldRegions, 1u);
	ASSERT_EQ(Debug.Bodies[0].HeightFieldNodeBoundsSample.size(), 1u);
	const auto& NodeBounds = Debug.Bodies[0].HeightFieldNodeBoundsSample[0];
	EXPECT_LE(NodeBounds[0].x, 0.0);
	EXPECT_LE(NodeBounds[0].y, 0.0);
	EXPECT_LE(NodeBounds[0].z, 0.0);
	EXPECT_GE(NodeBounds[1].x, 1.0);
	EXPECT_GE(NodeBounds[1].y, 1.0);
	EXPECT_GE(NodeBounds[1].z, 10.0);
	EXPECT_NEAR(NodeBounds[0].x, 0.0, 1.0e-6);
	EXPECT_NEAR(NodeBounds[1].z, 10.0, 1.0e-5);
	EXPECT_EQ(Debug.Bodies[0].TriangleSample.size(), 2u);
	const uint64 PreviousIdentity = FirstGeometry.GetIdentity();
	const std::array<uint16, 4> RaisedSamples{65535, 65535, 65535, 65535};
	ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, RaisedSamples, Error)) << Error;
	ASSERT_TRUE(First->RequestPhysicsStateCreation(true));
	ASSERT_TRUE(Second->RequestPhysicsStateCreation(true));
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 2u);
	ASSERT_TRUE(First->BuildCollisionGeometry(FirstGeometry, FirstTransform));
	EXPECT_NE(FirstGeometry.GetIdentity(), PreviousIdentity);
	ASSERT_TRUE(World->LineTraceSingleByChannel(Hit, {0.25, 0.25, 20.0},
		{0.25, 0.25, -20.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_NEAR(Hit.ImpactPoint.z, 10.0, 1.0e-8);
	First->SetHeightmap(nullptr);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);
	EXPECT_EQ(First->GetCollisionStatus(), Durin::ETerrainCollisionStatus::MissingHeightmap);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsTerrainTests, ReportsSeparatedBuildPhasesFor513And1025Fixtures)
{
	for (const uint32 Dimension : {513u, 1025u})
	{
		std::vector<uint16> Samples(static_cast<size_t>(Dimension) * Dimension);
		for (size_t Index = 0; Index < Samples.size(); ++Index)
			Samples[Index] = static_cast<uint16>((Index * 2654435761ull + Dimension) & 0xffffu);
		Durin::FCollisionGeometryBuildDiagnostics ColdDiagnostics;
		const Durin::FCollisionGeometryRef Geometry = Durin::FCollisionGeometryRef::BuildHeightField(
			Dimension, Dimension, Samples, 100.0, 100.0, 1000.0, -200.0, &ColdDiagnostics);
		ASSERT_TRUE(Geometry.IsValid());
		EXPECT_FALSE(ColdDiagnostics.bCacheHit);
		EXPECT_GT(ColdDiagnostics.HashNanoseconds, 0u);
		EXPECT_GT(ColdDiagnostics.MatchNanoseconds, 0u);
		EXPECT_GT(ColdDiagnostics.SampleCopyNanoseconds, 0u);
		EXPECT_GT(ColdDiagnostics.TreeBuildNanoseconds, 0u);

		Durin::FPhysicsScene Scene;
		Durin::FPhysicsBodyDesc Desc;
		Desc.Geometry = Geometry;
		const auto InsertionStart = std::chrono::steady_clock::now();
		ASSERT_TRUE(Scene.AddBody(Desc).IsValid());
		const uint64 InsertionNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - InsertionStart).count());
		EXPECT_GT(InsertionNanoseconds, 0u);

		Durin::FCollisionGeometryBuildDiagnostics WarmDiagnostics;
		const Durin::FCollisionGeometryRef WarmGeometry = Durin::FCollisionGeometryRef::BuildHeightField(
			Dimension, Dimension, Samples, 100.0, 100.0, 1000.0, -200.0, &WarmDiagnostics);
		ASSERT_TRUE(WarmGeometry.IsValid());
		EXPECT_TRUE(WarmDiagnostics.bCacheHit);
		EXPECT_EQ(WarmGeometry.GetIdentity(), Geometry.GetIdentity());
		std::cout << "TerrainCollisionFixture " << Dimension << 'x' << Dimension
			<< " hash_us=" << ColdDiagnostics.HashNanoseconds / 1000
			<< " match_us=" << ColdDiagnostics.MatchNanoseconds / 1000
			<< " copy_us=" << ColdDiagnostics.SampleCopyNanoseconds / 1000
			<< " tree_us=" << ColdDiagnostics.TreeBuildNanoseconds / 1000
			<< " insert_us=" << InsertionNanoseconds / 1000
			<< " warm_match_us=" << WarmDiagnostics.MatchNanoseconds / 1000 << '\n';
	}
}

TEST(FPhysicsTerrainTests, ReplacesSharedHeightmapAcrossTwoWorldsAndRetainsBodiesOnFailure)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(nullptr, "SharedWorldHeightmap");
	auto* FirstWorld = Durin::NewObject<Durin::DWorld>(nullptr, "TerrainFirstWorld");
	auto* SecondWorld = Durin::NewObject<Durin::DWorld>(nullptr, "TerrainSecondWorld");
	ASSERT_TRUE(FirstWorld->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(FirstWorld, "TerrainFirstLevel")));
	ASSERT_TRUE(SecondWorld->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(SecondWorld, "TerrainSecondLevel")));
	std::string Error;
	const std::array<uint16, 4> FlatSamples{0, 0, 0, 0};
	ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, FlatSamples, Error)) << Error;
	auto AddTerrain = [&](Durin::DWorld& World, std::string_view Name) {
		auto* Actor = World.SpawnActor<Durin::ATerrainActor>(Name);
		EXPECT_NE(Actor, nullptr);
		auto* Component = Actor ? Actor->GetTerrainComponent() : nullptr;
		EXPECT_NE(Component, nullptr);
		if (!Component) return Component;
		EXPECT_TRUE(Component->SetSampleSpacing(1.0, 1.0));
		EXPECT_TRUE(Component->SetHeightRange(10.0, 0.0));
		Component->SetHeightmap(Heightmap);
		EXPECT_TRUE(Component->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
		EXPECT_TRUE(Component->RequestPhysicsStateCreation(true));
		return Component;
	};
	Durin::DTerrainComponent* First = AddTerrain(*FirstWorld, "FirstWorldTerrain");
	Durin::DTerrainComponent* Second = AddTerrain(*SecondWorld, "SecondWorldTerrain");
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	ASSERT_EQ(FirstWorld->GetPhysicsScene().GetBodyCount(), 1u);
	ASSERT_EQ(SecondWorld->GetPhysicsScene().GetBodyCount(), 1u);
	Durin::FCollisionGeometryRef FirstGeometry;
	Durin::FCollisionGeometryRef SecondGeometry;
	Durin::FTransform Transform;
	ASSERT_TRUE(First->BuildCollisionGeometry(FirstGeometry, Transform));
	ASSERT_TRUE(Second->BuildCollisionGeometry(SecondGeometry, Transform));
	EXPECT_EQ(FirstGeometry.GetIdentity(), SecondGeometry.GetIdentity());
	const uint64 FlatIdentity = FirstGeometry.GetIdentity();
	const uint64 InitialRevision = Heightmap->GetRevision();
	const Durin::FPhysicsActorHandle FirstHandle = First->GetPhysicsActorHandle();
	const Durin::FPhysicsActorHandle SecondHandle = Second->GetPhysicsActorHandle();

	const std::array<uint16, 4> RaisedSamples{65535, 65535, 65535, 65535};
	ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, RaisedSamples, Error)) << Error;
	ASSERT_TRUE(First->RequestPhysicsStateCreation(true));
	ASSERT_TRUE(Second->RequestPhysicsStateCreation(true));
	EXPECT_GT(Heightmap->GetRevision(), InitialRevision);
	EXPECT_EQ(FirstWorld->GetPhysicsScene().GetBodyCount(), 1u);
	EXPECT_EQ(SecondWorld->GetPhysicsScene().GetBodyCount(), 1u);
	EXPECT_NE(First->GetPhysicsActorHandle(), FirstHandle);
	EXPECT_NE(Second->GetPhysicsActorHandle(), SecondHandle);
	ASSERT_TRUE(First->BuildCollisionGeometry(FirstGeometry, Transform));
	ASSERT_TRUE(Second->BuildCollisionGeometry(SecondGeometry, Transform));
	EXPECT_NE(FirstGeometry.GetIdentity(), FlatIdentity);
	EXPECT_EQ(FirstGeometry.GetIdentity(), SecondGeometry.GetIdentity());
	Durin::FHitResult Hit;
	ASSERT_TRUE(FirstWorld->LineTraceSingleByChannel(Hit, {0.5, 0.5, 20.0},
		{0.5, 0.5, -20.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_NEAR(Hit.ImpactPoint.z, 10.0, 1.0e-8);
	ASSERT_TRUE(SecondWorld->LineTraceSingleByChannel(Hit, {0.5, 0.5, 20.0},
		{0.5, 0.5, -20.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_NEAR(Hit.ImpactPoint.z, 10.0, 1.0e-8);

	const uint64 ReadyRevision = Heightmap->GetRevision();
	const uint64 ReadyIdentity = FirstGeometry.GetIdentity();
	const Durin::FPhysicsActorHandle ReadyFirstHandle = First->GetPhysicsActorHandle();
	const Durin::FPhysicsActorHandle ReadySecondHandle = Second->GetPhysicsActorHandle();
	const std::array<uint16, 1> InvalidSamples{0};
	EXPECT_FALSE(Heightmap->InitializeFromSamples(1, 1, InvalidSamples, Error));
	EXPECT_EQ(Heightmap->GetRevision(), ReadyRevision);
	EXPECT_EQ(First->GetPhysicsActorHandle(), ReadyFirstHandle);
	EXPECT_EQ(Second->GetPhysicsActorHandle(), ReadySecondHandle);
	EXPECT_EQ(FirstWorld->GetPhysicsScene().GetBodyCount(), 1u);
	EXPECT_EQ(SecondWorld->GetPhysicsScene().GetBodyCount(), 1u);
	ASSERT_TRUE(First->BuildCollisionGeometry(FirstGeometry, Transform));
	EXPECT_EQ(FirstGeometry.GetIdentity(), ReadyIdentity);

	First->SetHeightmap(nullptr);
	EXPECT_EQ(FirstWorld->GetPhysicsScene().GetBodyCount(), 0u);
	EXPECT_EQ(SecondWorld->GetPhysicsScene().GetBodyCount(), 1u);
	ASSERT_TRUE(SecondWorld->LineTraceSingleByChannel(Hit, {0.5, 0.5, 20.0},
		{0.5, 0.5, -20.0}, Durin::ECollisionChannel::Visibility));
	Durin::MarkObjectHierarchyAsGarbage(FirstWorld);
	Durin::MarkObjectHierarchyAsGarbage(SecondWorld);
	Durin::MarkObjectHierarchyAsGarbage(Heightmap);
	Durin::CollectGarbage();
}

TEST(FPhysicsTerrainTests, PropertyTransactionsCoalesceNoOpsRejectInvalidEditsAndRecover)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	const std::array<uint16, 4> Samples{0, 0, 0, 65535};
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(World, "PropertyTerrainHeightmap");
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, Samples, Error)) << Error;
	auto* Actor = World->SpawnActor<Durin::ATerrainActor>("PropertyTerrain");
	ASSERT_NE(Actor, nullptr);
	Durin::DTerrainComponent* Component = Actor->GetTerrainComponent();
	ASSERT_NE(Component, nullptr);
	ASSERT_TRUE(Component->SetSampleSpacing(2.0, 3.0));
	ASSERT_TRUE(Component->SetHeightRange(10.0, -2.0));
	Component->SetHeightmap(Heightmap);
	ASSERT_TRUE(Component->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(true));
	ASSERT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);

	Durin::FCollisionGeometryRef Geometry;
	Durin::FTransform Transform;
	ASSERT_TRUE(Component->BuildCollisionGeometry(Geometry, Transform));
	const uint64 InitialIdentity = Geometry.GetIdentity();
	const Durin::FPhysicsActorHandle InitialHandle = Component->GetPhysicsActorHandle();
	ASSERT_TRUE(Component->SetSampleSpacing(2.0, 3.0));
	ASSERT_TRUE(Component->SetHeightRange(10.0, -2.0));
	Component->SetHeightmap(Heightmap);
	EXPECT_EQ(Component->GetPhysicsActorHandle(), InitialHandle);
	ASSERT_TRUE(Component->BuildCollisionGeometry(Geometry, Transform));
	EXPECT_EQ(Geometry.GetIdentity(), InitialIdentity);

	EXPECT_FALSE(Component->SetSampleSpacing(0.0, 3.0));
	EXPECT_FALSE(Component->SetSampleSpacing(std::numeric_limits<double>::infinity(), 3.0));
	EXPECT_FALSE(Component->SetHeightRange(std::numeric_limits<double>::quiet_NaN(), -2.0));
	EXPECT_EQ(Component->GetPhysicsActorHandle(), InitialHandle);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);

	ASSERT_TRUE(Component->SetSampleSpacing(4.0, 3.0));
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(true));
	EXPECT_NE(Component->GetPhysicsActorHandle(), InitialHandle);
	const Durin::FPhysicsActorHandle SpacingHandle = Component->GetPhysicsActorHandle();
	ASSERT_TRUE(Component->BuildCollisionGeometry(Geometry, Transform));
	EXPECT_NE(Geometry.GetIdentity(), InitialIdentity);
	const uint64 SpacingIdentity = Geometry.GetIdentity();
	ASSERT_TRUE(Component->SetHeightRange(-10.0, 8.0));
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(true));
	EXPECT_NE(Component->GetPhysicsActorHandle(), SpacingHandle);
	ASSERT_TRUE(Component->BuildCollisionGeometry(Geometry, Transform));
	EXPECT_NE(Geometry.GetIdentity(), SpacingIdentity);
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Ready);

	Durin::FHitResult Hit;
	ASSERT_TRUE(World->LineTraceSingleByChannel(Hit, {3.0, 2.25, 20.0},
		{3.0, 2.25, -20.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_NEAR(Hit.ImpactPoint.z, 3.0, 1.0e-8);
	Component->SetHeightmap(nullptr);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 0u);
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::MissingHeightmap);
	Component->SetHeightmap(Heightmap);
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(true));
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Ready);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsTerrainTests, EditorRegistrationStaysDormantAndExplicitDebugRequestPublishesAsync)
{
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
	Durin::DWorld* World = CreatePhysicsWorld();
	World->SetWorldType(Durin::EWorldType::Editor);
	constexpr uint32 Dimension = 513;
	std::vector<uint16> Samples(static_cast<size_t>(Dimension) * Dimension, 32768);
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(World, "EditorDormantHeightmap");
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(Dimension, Dimension, Samples, Error)) << Error;
	auto* Actor = World->SpawnActor<Durin::ATerrainActor>("EditorDormantTerrain");
	ASSERT_NE(Actor, nullptr);
	Durin::DTerrainComponent* Component = Actor->GetTerrainComponent();
	ASSERT_NE(Component, nullptr);
	Component->SetHeightmap(Heightmap);
	ASSERT_TRUE(Component->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Dormant);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 0u);

	World->SetCollisionDebugDrawEnabled(true);
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Building);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 0u);
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(true));
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Ready);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);
	const Durin::FTerrainCollisionFacts InitialFacts = Component->GetCollisionFacts();
	EXPECT_GT(InitialFacts.HashNanoseconds, 0u);
	EXPECT_GT(InitialFacts.SampleCopyNanoseconds, 0u);
	EXPECT_GT(InitialFacts.TreeBuildNanoseconds, 0u);
	EXPECT_GT(InitialFacts.PhysicsInsertionNanoseconds, 0u);

	World->SetWorldType(Durin::EWorldType::PlayInEditor);
	ASSERT_TRUE(Component->SetSampleSpacing(2.0, 2.0));
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Building);
	const Durin::FWorldPlayResult PlayResult = World->BeginPlay({});
	ASSERT_TRUE(PlayResult) << PlayResult.Message;
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Ready);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);
	World->EndPlay();

	World->SetWorldType(Durin::EWorldType::Editor);
	ASSERT_TRUE(Component->SetHeightRange(500.0, 10.0));
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Dormant);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 0u);
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(false));
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Building);
	Component->SetCollisionResponseToChannel(
		Durin::ECollisionChannel::Pawn, Durin::ECollisionResponse::Ignore);
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Dormant);
	ASSERT_TRUE(Component->RequestPhysicsStateCreation(false));
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Building);
	ASSERT_TRUE(World->SetCurrentLevel(
		Durin::NewObject<Durin::DLevel>(World, "ReplacementAfterTerrainBuild")));
	EXPECT_EQ(Component->GetCollisionStatus(), Durin::ETerrainCollisionStatus::Unavailable);
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 0u);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
	Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Cancel);
}

TEST(FPhysicsTerrainTests, BeginPlayRejectsMissingRequiredTerrainCollision)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	auto* Actor = World->SpawnActor<Durin::ATerrainActor>("MissingCollisionTerrain");
	ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Actor->GetTerrainComponent()->SetCollisionProfileName(
		Durin::CollisionProfile::WorldStatic));
	const Durin::FWorldPlayResult Result = World->BeginPlay({});
	EXPECT_EQ(Result.Error, Durin::EWorldPlayError::CollisionNotReady);
	EXPECT_FALSE(Result.Message.empty());
	EXPECT_FALSE(World->HasBegunPlay());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsSceneTests, AppliesTwoSidedFiltersIgnoresAndStableTieBreaking)
{
	Durin::FPhysicsScene Scene;
	Durin::FPhysicsBodyDesc First = MakeBoxBody({0.0, 0.0, 0.0});
	Durin::FPhysicsBodyDesc Second = First;
	const Durin::FPhysicsActorHandle FirstHandle = Scene.AddBody(First);
	const Durin::FPhysicsActorHandle SecondHandle = Scene.AddBody(Second);
	ASSERT_TRUE(FirstHandle.IsValid());
	ASSERT_TRUE(SecondHandle.IsValid());
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {}, Hit));
	EXPECT_EQ(Hit.ActorHandle, FirstHandle);
	Durin::FPhysicsQueryFilter Ignored;
	Ignored.IgnoredActors.push_back(FirstHandle);
	ASSERT_TRUE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Ignored, Hit));
	EXPECT_EQ(Hit.ActorHandle, SecondHandle);
	Second.Filter.Responses[0] = Durin::EPhysicsQueryResponse::Ignore;
	ASSERT_TRUE(Scene.UpdateBody(SecondHandle, Second));
	EXPECT_FALSE(Scene.LineTraceSingle({-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Ignored, Hit));
	EXPECT_TRUE(Scene.RemoveBody(SecondHandle));
	EXPECT_FALSE(Scene.RemoveBody(SecondHandle));
}

TEST(FPhysicsSceneTests, RejectsOffThreadAndNonFiniteInputWithoutMutation)
{
	Durin::FPhysicsScene Scene;
	const Durin::FPhysicsActorHandle Handle = Scene.AddBody(MakeBoxBody({0.0, 0.0, 0.0}));
	ASSERT_TRUE(Handle.IsValid());
	bool bRemoved = true;
	bool bQueried = true;
	std::thread Worker([&] {
		Durin::FPhysicsQueryHit Hit;
		bRemoved = Scene.RemoveBody(Handle);
		bQueried = Scene.LineTraceSingle({-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {}, Hit);
	});
	Worker.join();
	EXPECT_FALSE(bRemoved);
	EXPECT_FALSE(bQueried);
	EXPECT_TRUE(Scene.ContainsBody(Handle));
	Durin::FPhysicsQueryHit Hit;
	EXPECT_FALSE(Scene.LineTraceSingle(
		{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, {1.0, 0.0, 0.0}, {}, Hit));
	EXPECT_TRUE(Scene.ContainsBody(Handle));
}

TEST(FPhysicsWorldTests, SynchronizesComponentLifecycleTransformAndWorldIsolation)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	Durin::DWorld* OtherWorld = CreatePhysicsWorld();
	Durin::DBoxComponent* Box = AddWorldBox(*World, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
	ASSERT_EQ(World->GetPhysicsScene().GetBodyCount(), 1u);
	EXPECT_EQ(OtherWorld->GetPhysicsScene().GetBodyCount(), 0u);
	Durin::FHitResult Hit;
	ASSERT_TRUE(World->LineTraceSingleByChannel(
		Hit, {-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, Durin::ECollisionChannel::Visibility));
	EXPECT_EQ(Hit.Component, Box);
	Box->SetWorldLocation({10.0, 0.0, 0.0});
	EXPECT_FALSE(World->LineTraceSingleByChannel(
		Hit, {-3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}, Durin::ECollisionChannel::Visibility));
	Box->UnregisterComponent();
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 0u);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkObjectHierarchyAsGarbage(OtherWorld);
	Durin::CollectGarbage();
}

TEST(FPhysicsWorldTests, QueriesRemainAvailableWhenSimulationIsDisabledOrPaused)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	AddWorldBox(*World, {0.0, 0.0, -0.5}, {5.0, 5.0, 0.5});
	World->SetPhysicsSimulationEnabled(false);
	World->SetPaused(true);
	Durin::FHitResult Hit;
	EXPECT_TRUE(World->LineTraceSingleByChannel(
		Hit, {0.0, 0.0, 2.0}, {0.0, 0.0, -2.0}, Durin::ECollisionChannel::Visibility));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsWorldTests, CollisionDebugSnapshotIsBoundedAndDisabledByDefault)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	Durin::DBoxComponent* Box = AddWorldBox(*World, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
	EXPECT_TRUE(World->CaptureCollisionDebugSnapshot().Bodies.empty());
	World->SetCollisionDebugDrawEnabled(true);
	Durin::FHitResult Hit;
	ASSERT_TRUE(World->LineTraceSingleByChannel(
		Hit, {-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, Durin::ECollisionChannel::Visibility));
	const Durin::FCollisionDebugSnapshot Snapshot = World->CaptureCollisionDebugSnapshot();
	ASSERT_EQ(Snapshot.Bodies.size(), 1u);
	EXPECT_EQ(Snapshot.Bodies.front().Component, Box);
	EXPECT_TRUE(Snapshot.LastBlockingHit.has_value());
	std::vector<Durin::FVector3> DebugVertices;
	std::vector<uint32> DebugIndices;
	for (uint32 Index = 0; Index < 300; ++Index)
	{
		const uint32 First = static_cast<uint32>(DebugVertices.size());
		const double X = static_cast<double>(Index) * 2.0;
		const double Z = static_cast<double>(Index & 1u);
		DebugVertices.insert(DebugVertices.end(), {{X, 0.0, Z}, {X + 1.0, 0.0, Z}, {X, 1.0, Z}});
		DebugIndices.insert(DebugIndices.end(), {First, First + 1, First + 2});
	}
	Durin::FPhysicsBodyDesc FeatureBody;
	FeatureBody.Geometry = Durin::FCollisionGeometryRef::BuildTriangleMesh(DebugVertices, DebugIndices);
	FeatureBody.Transform = Durin::FTransform();
	FeatureBody.UserToken = reinterpret_cast<uint64>(Box);
	ASSERT_TRUE(World->GetPhysicsScene().AddBody(FeatureBody).IsValid());
	const Durin::FCollisionDebugSnapshot FeatureSnapshot = World->CaptureCollisionDebugSnapshot();
	const auto Feature = std::ranges::find_if(FeatureSnapshot.Bodies, [](const Durin::FCollisionDebugBody& Body) {
		return Body.GeometryKind == Durin::ECollisionGeometryKind::TriangleMesh;
	});
	ASSERT_NE(Feature, FeatureSnapshot.Bodies.end());
	EXPECT_FALSE(Feature->bHasPrimitiveShape);
	EXPECT_EQ(Feature->TotalTriangles, 300u);
	EXPECT_EQ(Feature->TriangleSample.size(), 256u);
	Durin::FPhysicsBodyDesc DebugBody = MakeBoxBody({10.0, 0.0, 0.0});
	DebugBody.UserToken = static_cast<decltype(DebugBody.UserToken)>(reinterpret_cast<std::uintptr_t>(Box));
	for (size_t Index = 0; Index < 4096; ++Index)
	{
		DebugBody.Transform.Translation.x = 10.0 + static_cast<double>(Index);
		ASSERT_TRUE(World->GetPhysicsScene().AddBody(DebugBody).IsValid());
	}
	EXPECT_EQ(World->CaptureCollisionDebugSnapshot().Bodies.size(), 4096u);
	World->SetCollisionDebugDrawEnabled(false);
	EXPECT_TRUE(World->CaptureCollisionDebugSnapshot().Bodies.empty());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsWorldTests, StaticMeshCollisionPolicyRepublishesSharedSceneGeometry)
{
	Durin::FModuleManager::Get().LoadModule("StaticMeshBuild");
	Durin::DWorld* FirstWorld = CreatePhysicsWorld();
	Durin::DWorld* SecondWorld = CreatePhysicsWorld();
	std::string Error;
	Durin::DStaticMesh* Mesh = Durin::NewObject<Durin::DStaticMesh>(FirstWorld, "SceneCollisionMesh");
	Durin::FStaticMeshImportedData Imported;
	Imported.MaterialSlots.push_back({"Default", 0, "Default"});
	Durin::FStaticMeshImportedMesh& ImportedMesh = Imported.Meshes.emplace_back();
	ImportedMesh.Name = "Tetrahedron";
	ImportedMesh.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	ImportedMesh.Indices = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3};
	ImportedMesh.SourceMaterialIndex = 0;
	ASSERT_TRUE(Durin::BuildStaticMeshSynchronously(
		*Mesh, Imported, Error)) << Error;
	ASSERT_TRUE(Mesh->SetCollisionSourceMode(
		Durin::EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	auto AddMesh = [&](Durin::DWorld& World, std::string_view Name) {
		auto* Actor = World.SpawnActor<Durin::AStaticMeshActor>(Durin::FName(Name));
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		return Actor->GetStaticMeshComponent();
	};
	Durin::DStaticMeshComponent* First = AddMesh(*FirstWorld, "FirstCollisionOwner");
	Durin::DStaticMeshComponent* Second = AddMesh(*SecondWorld, "SecondCollisionOwner");
	ASSERT_TRUE(First->GetPhysicsActorHandle().IsValid());
	ASSERT_TRUE(Second->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(First->GetCollisionProfileName(), Durin::CollisionProfile::WorldStatic);
	EXPECT_EQ(First->GetPublishedBodySetupRevision(), Mesh->GetBodySetup()->GetRevision());
	const auto FirstBodies = FirstWorld->GetPhysicsScene().CaptureBodies();
	const auto SecondBodies = SecondWorld->GetPhysicsScene().CaptureBodies();
	ASSERT_EQ(FirstBodies.size(), 1u);
	ASSERT_EQ(SecondBodies.size(), 1u);
	EXPECT_EQ(FirstBodies.front().Desc.Geometry.GetIdentity(), SecondBodies.front().Desc.Geometry.GetIdentity());
	EXPECT_EQ(FirstWorld->GetPhysicsScene().CaptureQueryDiagnostics().Mutations.UniqueGeometryResources, 1u);
	EXPECT_EQ(SecondWorld->GetPhysicsScene().CaptureQueryDiagnostics().Mutations.UniqueGeometryResources, 1u);
	const std::optional<Durin::FBox> Bounds = Mesh->GetLOD0LocalBounds();
	ASSERT_TRUE(Bounds.has_value());
	const Durin::FVector3 Center = Bounds->GetCenter();
	Durin::FHitResult Hit;
	ASSERT_TRUE(FirstWorld->LineTraceSingleByChannel(
		Hit, {Bounds->Min.x - 1.0, Center.y, Center.z},
		{Bounds->Max.x + 1.0, Center.y, Center.z}, Durin::ECollisionChannel::Visibility));
	EXPECT_EQ(Hit.Component, First);

	ASSERT_TRUE(Mesh->SetCollisionQueryPolicy(
		Durin::EBodySetupCollisionQueryPolicy::SimpleOnly, Error)) << Error;
	EXPECT_FALSE(First->GetPhysicsActorHandle().IsValid());
	EXPECT_FALSE(Second->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(First->GetCollisionProfileName(), Durin::CollisionProfile::WorldStatic);
	ASSERT_TRUE(Mesh->SetCollisionQueryPolicy(
		Durin::EBodySetupCollisionQueryPolicy::ComplexOnly, Error)) << Error;
	EXPECT_TRUE(First->GetPhysicsActorHandle().IsValid());
	EXPECT_TRUE(Second->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(First->GetPublishedBodySetupRevision(), Mesh->GetBodySetup()->GetRevision());

	First->SetStaticMesh(nullptr);
	EXPECT_FALSE(First->GetPhysicsActorHandle().IsValid());
	EXPECT_TRUE(Second->GetPhysicsActorHandle().IsValid());
	Durin::MarkObjectHierarchyAsGarbage(FirstWorld);
	Durin::MarkObjectHierarchyAsGarbage(SecondWorld);
	Durin::CollectGarbage();
}

TEST(FPhysicsBodySetupTests, SharesAssetGeometryWhileInstancesKeepDistinctHandles)
{
	Durin::DWorld* World = CreatePhysicsWorld();
	auto* Mesh = Durin::NewObject<Durin::DStaticMesh>(World, "SharedCollisionMesh");
	auto* Setup = Durin::NewObject<Durin::DBodySetup>(Mesh, "BodySetup");
	ASSERT_TRUE(Setup->SetBox({0.5, 0.5, 0.5}));
	ASSERT_TRUE(Mesh->SetBodySetup(Setup));
	auto* First = World->SpawnActor<Durin::AStaticMeshActor>("First");
	auto* Second = World->SpawnActor<Durin::AStaticMeshActor>("Second");
	First->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Second->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Second->GetStaticMeshComponent()->SetWorldLocation({2.0, 0.0, 0.0});
	EXPECT_EQ(First->GetStaticMeshComponent()->GetPhysicsBodyMotionType(),
		Durin::EPhysicsBodyMotionType::Static);
	EXPECT_EQ(Second->GetStaticMeshComponent()->GetPhysicsBodyMotionType(),
		Durin::EPhysicsBodyMotionType::Static);
	EXPECT_EQ(First->GetStaticMeshComponent()->GetBodySetup(), Setup);
	EXPECT_EQ(Second->GetStaticMeshComponent()->GetBodySetup(), Setup);
	EXPECT_NE(First->GetStaticMeshComponent()->GetPhysicsActorHandle(),
		Second->GetStaticMeshComponent()->GetPhysicsActorHandle());
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 2u);
	const std::vector<Durin::FPhysicsBodySnapshot> SharedBodies = World->GetPhysicsScene().CaptureBodies();
	ASSERT_EQ(SharedBodies.size(), 2u);
	EXPECT_EQ(SharedBodies[0].Desc.Geometry.GetIdentity(), SharedBodies[1].Desc.Geometry.GetIdentity());
	auto* RenderOnlyOwner = World->SpawnActor<Durin::ACameraActor>("RenderOnlyOwner");
	auto* RenderOnly = Durin::Cast<Durin::DStaticMeshComponent>(
		RenderOnlyOwner->AddInstanceComponent(Durin::DStaticMeshComponent::StaticClass(), "RenderOnly"));
	ASSERT_NE(RenderOnly, nullptr);
	RenderOnly->SetStaticMesh(Mesh);
	EXPECT_EQ(RenderOnly->GetCollisionEnabled(), Durin::ECollisionEnabled::NoCollision);
	EXPECT_FALSE(RenderOnly->GetPhysicsActorHandle().IsValid());
	EXPECT_EQ(World->GetPhysicsScene().GetBodyCount(), 2u);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FPhysicsCollisionGeometryResourceTests, ValidatesIdentityBoundsChildrenAndRetainedBytes)
{
	static_assert(sizeof(Durin::FCollisionGeometryRef) == 16);
	static_assert(sizeof(Durin::FCollisionGeometryChild) == 112);
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakePrimitive({}).IsValid());
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeCompound({}).IsValid());
	std::vector<Durin::FCollisionGeometryChild> Children(65);
	for (Durin::FCollisionGeometryChild& Child : Children)
		Child.Shape = Durin::FCollisionShape::MakeSphere(0.5);
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeCompound(Children).IsValid());

	Children.resize(64);
	for (uint32 Index = 0; Index < Children.size(); ++Index)
	{
		Children[Index].LocalTransform.Translation = {static_cast<double>(Index), 0.0, 0.0};
	}
	const Durin::FCollisionGeometryRef Compound = Durin::FCollisionGeometryRef::MakeCompound(Children);
	ASSERT_TRUE(Compound.IsValid());
	EXPECT_NE(Compound.GetIdentity(), 0u);
	EXPECT_EQ(Compound.GetChildCount(), 64u);
	EXPECT_EQ(Compound.GetRetainedBytes(), ExpectedCompoundRetainedBytes);
	Durin::FVector3 Min;
	Durin::FVector3 Max;
	ASSERT_TRUE(Compound.GetLocalBounds(Min, Max));
	EXPECT_DOUBLE_EQ(Min.x, -0.5);
	EXPECT_DOUBLE_EQ(Max.x, 63.5);
	EXPECT_DOUBLE_EQ(Compound.GetChild(17)->LocalTransform.Translation.x, 17.0);

	Children[4].LocalTransform.Scale3D.x = 0.0;
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeCompound(Children).IsValid());
}

TEST(FPhysicsBodySetupTests, CachesOneGeometryIdentityPerAuthoredRevision)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	auto* Setup = Durin::NewObject<Durin::DBodySetup>(nullptr, "CachedGeometrySetup");
	ASSERT_TRUE(Setup->SetCapsule(0.5, 1.5, {1.0, 2.0, 3.0}));
	Durin::FCollisionGeometryRef First;
	Durin::FCollisionGeometryRef Second;
	Durin::FTransform FirstTransform;
	Durin::FTransform SecondTransform;
	ASSERT_TRUE(Setup->BuildGeometry(First, FirstTransform));
	ASSERT_TRUE(Setup->BuildGeometry(Second, SecondTransform));
	EXPECT_EQ(First.GetIdentity(), Second.GetIdentity());
	EXPECT_EQ(FirstTransform.Translation, Durin::FVector3(1.0, 2.0, 3.0));
	ASSERT_TRUE(Setup->SetSphere(2.0));
	ASSERT_TRUE(Setup->BuildGeometry(Second, SecondTransform));
	EXPECT_NE(First.GetIdentity(), Second.GetIdentity());
	Durin::MarkObjectHierarchyAsGarbage(Setup);
	Durin::CollectGarbage();
}

TEST(FPhysicsNarrowphaseMatrixTests, ReachesEveryPrimitiveRaySweepAndOverlapCell)
{
	const std::array<Durin::FCollisionShape, 3> Shapes{
		Durin::FCollisionShape::MakeBox({0.75, 0.75, 0.75}),
		Durin::FCollisionShape::MakeSphere(0.75),
		Durin::FCollisionShape::MakeCapsule(0.5, 1.0)};
	for (const Durin::FCollisionShape& TargetShape : Shapes)
	{
		const Durin::FCollisionGeometryRef Target =
			Durin::FCollisionGeometryRef::MakePrimitive(TargetShape);
		Durin::FPhysicsQueryHit Hit;
		EXPECT_EQ(Durin::CollisionGeometry::Raycast(
			{-5.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, Target, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		for (const Durin::FCollisionShape& QueryShape : Shapes)
		{
			Durin::FTransform OverlapTransform;
			EXPECT_EQ(Durin::CollisionGeometry::Overlap(
				QueryShape, OverlapTransform, Target, Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			Durin::FTransform SweepTransform;
			SweepTransform.Translation = {-5.0, 0.0, 0.0};
			EXPECT_EQ(Durin::CollisionGeometry::Sweep(
				QueryShape, SweepTransform, {10.0, 0.0, 0.0}, Target, Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Hit),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_GE(Hit.Time, 0.0);
			EXPECT_LE(Hit.Time, 1.0);
			EXPECT_NEAR(Durin::Math::Length(Hit.ImpactNormal), 1.0, 1.0e-8);
		}
	}
}

TEST(FPhysicsNarrowphaseMatrixTests, CompoundUsesStableChildOrderAndOneBodyResult)
{
	std::array<Durin::FCollisionGeometryChild, 2> Children{};
	Children[0].Shape = Durin::FCollisionShape::MakeSphere(0.5);
	Children[0].LocalTransform.Translation = {2.0, 0.0, 0.0};
	Children[1].Shape = Durin::FCollisionShape::MakeBox({0.5, 0.5, 0.5});
	Children[1].LocalTransform.Translation = {-2.0, 0.0, 0.0};
	Durin::FPhysicsBodyDesc Body;
	Body.Geometry = Durin::FCollisionGeometryRef::MakeCompound(Children);
	Durin::FPhysicsScene Scene;
	ASSERT_TRUE(Scene.AddBody(Body).IsValid());
	Durin::FPhysicsQueryHit Hit;
	ASSERT_TRUE(Scene.LineTraceSingle({-5.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, {}, Hit));
	EXPECT_NEAR(Hit.Time, 0.25, 1.0e-12);
	std::vector<Durin::FPhysicsQueryHit> Hits;
	ASSERT_TRUE(Scene.OverlapMulti(
		Durin::FCollisionShape::MakeBox({3.0, 1.0, 1.0}), Durin::FTransform(), {}, Hits));
	EXPECT_EQ(Hits.size(), 1u);
}

namespace
{
	auto MakeStage1CubeHull() -> Durin::FCollisionGeometryRef
	{
		const std::array<Durin::FVector3, 8> Vertices{
			Durin::FVector3{-1.0, -1.0, -1.0}, Durin::FVector3{1.0, -1.0, -1.0},
			Durin::FVector3{-1.0, 1.0, -1.0}, Durin::FVector3{1.0, 1.0, -1.0},
			Durin::FVector3{-1.0, -1.0, 1.0}, Durin::FVector3{1.0, -1.0, 1.0},
			Durin::FVector3{-1.0, 1.0, 1.0}, Durin::FVector3{1.0, 1.0, 1.0}};
		const std::array<uint32, 36> Indices{
			0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5,
			0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6,
			0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3};
		return Durin::FCollisionGeometryRef::MakeConvexHull(Vertices, Indices);
	}

	auto MakeStage1TrianglePlane() -> Durin::FCollisionGeometryRef
	{
		const std::array<Durin::FVector3, 4> Vertices{
			Durin::FVector3{0.0, -3.0, -3.0}, Durin::FVector3{0.0, 3.0, -3.0},
			Durin::FVector3{0.0, 3.0, 3.0}, Durin::FVector3{0.0, -3.0, 3.0}};
		const std::array<uint32, 6> Indices{0, 1, 2, 0, 2, 3};
		const std::array<uint32, 2> Ordinals{17, 42};
		return Durin::FCollisionGeometryRef::MakeTriangleMesh(Vertices, Indices, Ordinals);
	}
}

TEST(FPhysicsCollisionGeometryStage1Tests, ValidatesImmutableFeatureResourcesAndStableAccess)
{
	static_assert(sizeof(Durin::FCollisionGeometryRef) == 16);
	static_assert(sizeof(Durin::FCollisionGeometryTriangle) == 16);
	const Durin::FCollisionGeometryRef Hull = MakeStage1CubeHull();
	ASSERT_TRUE(Hull.IsValid());
	EXPECT_EQ(Hull.GetKind(), Durin::ECollisionGeometryKind::ConvexHull);
	EXPECT_EQ(Hull.GetChildCount(), 0u);
	EXPECT_EQ(Hull.GetVertexCount(), 8u);
	EXPECT_EQ(Hull.GetTriangleCount(), 12u);
	ASSERT_NE(Hull.GetTriangle(0), nullptr);
	EXPECT_EQ(Hull.GetTriangle(0)->SourceOrdinal, 0u);
	EXPECT_EQ(*Hull.GetVertex(7), Durin::FVector3(1.0));
	EXPECT_EQ(Hull.GetVertex(8), nullptr);
	EXPECT_EQ(Hull.GetTriangle(12), nullptr);
	Durin::FVector3 Minimum;
	Durin::FVector3 Maximum;
	ASSERT_TRUE(Hull.GetLocalBounds(Minimum, Maximum));
	EXPECT_EQ(Minimum, Durin::FVector3(-1.0));
	EXPECT_EQ(Maximum, Durin::FVector3(1.0));
	EXPECT_GT(Hull.GetIdentity(), 0u);
	EXPECT_GE(Hull.GetRetainedBytes(), 8u * sizeof(Durin::FVector3)
		+ 12u * sizeof(Durin::FCollisionGeometryTriangle));

	const Durin::FCollisionGeometryRef Mesh = MakeStage1TrianglePlane();
	ASSERT_TRUE(Mesh.IsValid());
	EXPECT_EQ(Mesh.GetKind(), Durin::ECollisionGeometryKind::TriangleMesh);
	ASSERT_NE(Mesh.GetTriangle(1), nullptr);
	EXPECT_EQ(Mesh.GetTriangle(0)->SourceOrdinal, 17u);
	EXPECT_EQ(Mesh.GetTriangle(1)->SourceOrdinal, 42u);
	EXPECT_NE(Hull.GetIdentity(), Mesh.GetIdentity());

	const Durin::FCollisionGeometryRef Primitive = Durin::FCollisionGeometryRef::MakePrimitive(
		Durin::FCollisionShape::MakeBox({0.5, 0.5, 0.5}));
	EXPECT_EQ(Primitive.GetKind(), Durin::ECollisionGeometryKind::Primitive);
	EXPECT_EQ(Primitive.GetRetainedBytes(), ExpectedPrimitiveRetainedBytes);
}

TEST(FPhysicsCollisionGeometryStage1Tests, RejectsMalformedFeatureResourcesTransactionally)
{
	const std::array<Durin::FVector3, 4> Tetra{
		Durin::FVector3{0.0, 0.0, 0.0}, Durin::FVector3{1.0, 0.0, 0.0},
		Durin::FVector3{0.0, 1.0, 0.0}, Durin::FVector3{0.0, 0.0, 1.0}};
	const std::array<uint32, 12> Closed{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	EXPECT_TRUE(Durin::FCollisionGeometryRef::MakeConvexHull(Tetra, Closed).IsValid());
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeConvexHull(Tetra,
		std::span(Closed).first(9)).IsValid());
	std::array<uint32, 12> Inconsistent = Closed;
	std::swap(Inconsistent[1], Inconsistent[2]);
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeConvexHull(Tetra, Inconsistent).IsValid());
	std::array<Durin::FVector3, 257> Oversized{};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeConvexHull(Oversized, Closed).IsValid());
	std::array<Durin::FVector3, 3> Degenerate{
		Durin::FVector3{0.0}, Durin::FVector3{1.0, 0.0, 0.0}, Durin::FVector3{2.0, 0.0, 0.0}};
	const std::array<uint32, 3> Triangle{0, 1, 2};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeTriangleMesh(Degenerate, Triangle).IsValid());
	Degenerate[2] = {0.0, 1.0, 0.0};
	const std::array<uint32, 1> WrongOrdinals{1};
	const std::array<uint32, 2> TooManyOrdinals{1, 2};
	EXPECT_TRUE(Durin::FCollisionGeometryRef::MakeTriangleMesh(
		Degenerate, Triangle, WrongOrdinals).IsValid());
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeTriangleMesh(
		Degenerate, Triangle, TooManyOrdinals).IsValid());
	Degenerate[0].x = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(Durin::FCollisionGeometryRef::MakeTriangleMesh(Degenerate, Triangle).IsValid());
}

TEST(FPhysicsCollisionGeometryStage1Tests, ReferenceMatrixCoversHullAndTriangleTargets)
{
	const std::array<Durin::FCollisionGeometryRef, 2> Targets{
		MakeStage1CubeHull(), MakeStage1TrianglePlane()};
	const std::array<Durin::FCollisionShape, 3> Queries{
		Durin::FCollisionShape::MakeBox({0.25, 0.25, 0.25}),
		Durin::FCollisionShape::MakeSphere(0.25),
		Durin::FCollisionShape::MakeCapsule(0.25, 0.5)};
	for (uint32 TargetIndex = 0; TargetIndex < Targets.size(); ++TargetIndex)
	{
		ASSERT_TRUE(Targets[TargetIndex].IsValid());
		for (const Durin::FCollisionShape& Query : Queries)
		{
			Durin::CollisionGeometry::FCollisionGeometryCounters Counters;
			Durin::FPhysicsQueryHit Hit;
			const Durin::FVector3 RayStart = TargetIndex == 0
				? Durin::FVector3{-3.0, 0.0, 0.0} : Durin::FVector3{-2.0, 0.0, 0.0};
			const Durin::FVector3 RayEnd = -RayStart;
			EXPECT_EQ(Durin::CollisionGeometry::Raycast(
				RayStart, RayEnd, Targets[TargetIndex], Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &Counters),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_NEAR(Hit.Time, TargetIndex == 0 ? 1.0 / 3.0 : 0.5, 1.0e-8);
			EXPECT_TRUE(Durin::Math::IsFinite(Hit.ImpactNormal));

			Durin::FTransform SweepTransform;
			SweepTransform.Translation = RayStart;
			EXPECT_EQ(Durin::CollisionGeometry::Sweep(
				Query, SweepTransform, RayEnd - RayStart, Targets[TargetIndex], Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &Counters),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_GT(Hit.Time, 0.0);
			EXPECT_LT(Hit.Time, 1.0);
			EXPECT_NEAR(Durin::Math::Length(Hit.ImpactNormal), 1.0, 1.0e-8);

			Durin::FTransform OverlapTransform;
			OverlapTransform.Translation = TargetIndex == 0
				? Durin::FVector3{-0.9, 0.0, 0.0} : Durin::FVector3{-0.1, 0.0, 0.0};
			EXPECT_EQ(Durin::CollisionGeometry::Overlap(
				Query, OverlapTransform, Targets[TargetIndex], Durin::FTransform(),
				Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Hit, &Counters),
				Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
			EXPECT_TRUE(Hit.bStartPenetrating);
			EXPECT_GT(Hit.PenetrationDepth, 0.0);
			EXPECT_GT(Counters.FeatureTests, 0u);
			EXPECT_EQ(Counters.Unsupported, 0u);
			EXPECT_EQ(Counters.NonConverged, 0u);
			EXPECT_FALSE(Counters.bOverflowed);
		}
	}
}

TEST(FPhysicsCollisionGeometryStage1Tests, AppliesRandomizedPositiveTransformsWithStableReferenceProductionParity)
{
	const Durin::FCollisionGeometryRef Target = MakeStage1CubeHull();
	std::mt19937_64 Random(0x5341474531ull);
	std::uniform_real_distribution<double> Position(-20.0, 20.0);
	std::uniform_real_distribution<double> Scale(0.25, 3.0);
	std::uniform_real_distribution<double> Angle(-180.0, 180.0);
	for (uint32 Iteration = 0; Iteration < 128; ++Iteration)
	{
		Durin::FTransform Transform;
		Transform.Translation = {Position(Random), Position(Random), Position(Random)};
		Transform.Scale3D = {Scale(Random), Scale(Random), Scale(Random)};
		Transform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
			Angle(Random), Durin::Math::NormalizeOr(
				Durin::FVector3{Position(Random), Position(Random), Position(Random)},
				Durin::FVectorConstants::Up));
		const Durin::FVector3 Axis = Durin::Math::RotateVector(
			Transform.Rotation, Durin::FVectorConstants::Forward);
		const Durin::FVector3 Start = Transform.Translation - Axis * (Transform.Scale3D.x + 2.0);
		const Durin::FVector3 End = Transform.Translation + Axis * (Transform.Scale3D.x + 2.0);
		Durin::FPhysicsQueryHit Reference;
		Durin::FPhysicsQueryHit Production;
		ASSERT_EQ(Durin::CollisionGeometry::Raycast(Start, End, Target, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		ASSERT_EQ(Durin::CollisionGeometry::Raycast(Start, End, Target, Transform,
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_DOUBLE_EQ(Reference.Time, Production.Time);
		EXPECT_EQ(Reference.ImpactNormal, Production.ImpactNormal);
		EXPECT_TRUE(Durin::Math::IsFinite(Reference.Location));
	}
}

TEST(FPhysicsCollisionGeometryStage2Tests, BuildsDeterministicHullAndMeshTopology)
{
	static_assert(sizeof(Durin::FCollisionGeometryNode) == 32);
	static_assert(sizeof(Durin::FCollisionHullPlane) == 16);
	static_assert(sizeof(Durin::FCollisionHullHalfEdge) == 16);
	static_assert(sizeof(Durin::FCollisionHullFace) == 16);
	std::array<Durin::FVector3, 10> Points{
		Durin::FVector3{-1.0, -1.0, -1.0}, Durin::FVector3{1.0, -1.0, -1.0},
		Durin::FVector3{-1.0, 1.0, -1.0}, Durin::FVector3{1.0, 1.0, -1.0},
		Durin::FVector3{-1.0, -1.0, 1.0}, Durin::FVector3{1.0, -1.0, 1.0},
		Durin::FVector3{-1.0, 1.0, 1.0}, Durin::FVector3{1.0, 1.0, 1.0},
		Durin::FVector3{0.0, 0.0, 0.0}, Durin::FVector3{1.0, 1.0, 1.0}};
	Durin::FCollisionGeometryBuildDiagnostics FirstFacts;
	const Durin::FCollisionGeometryRef First =
		Durin::FCollisionGeometryRef::BuildConvexHull(Points, &FirstFacts);
	ASSERT_TRUE(First.IsValid());
	EXPECT_EQ(FirstFacts.Status, Durin::ECollisionGeometryBuildStatus::Success);
	EXPECT_EQ(FirstFacts.RetainedVertices, 8u);
	EXPECT_EQ(First.GetVertexCount(), 8u);
	EXPECT_EQ(First.GetTriangleCount(), 12u);
	EXPECT_EQ(First.GetHullPlaneCount(), 12u);
	EXPECT_EQ(First.GetHullFaceCount(), 12u);
	EXPECT_EQ(First.GetHullHalfEdgeCount(), 36u);
	for (uint32 EdgeIndex = 0; EdgeIndex < First.GetHullHalfEdgeCount(); ++EdgeIndex)
	{
		const Durin::FCollisionHullHalfEdge* Edge = First.GetHullHalfEdge(EdgeIndex);
		ASSERT_NE(Edge, nullptr);
		ASSERT_LT(Edge->Twin, First.GetHullHalfEdgeCount());
		EXPECT_EQ(First.GetHullHalfEdge(Edge->Twin)->Twin, EdgeIndex);
		EXPECT_LT(Edge->Next, First.GetHullHalfEdgeCount());
		EXPECT_LT(Edge->Face, First.GetHullFaceCount());
	}
	std::reverse(Points.begin(), Points.end());
	Durin::FCollisionGeometryBuildDiagnostics SecondFacts;
	const Durin::FCollisionGeometryRef Second =
		Durin::FCollisionGeometryRef::BuildConvexHull(Points, &SecondFacts);
	ASSERT_TRUE(Second.IsValid());
	ASSERT_EQ(First.GetVertexCount(), Second.GetVertexCount());
	ASSERT_EQ(First.GetTriangleCount(), Second.GetTriangleCount());
	for (uint32 Index = 0; Index < First.GetVertexCount(); ++Index)
		EXPECT_EQ(*First.GetVertex(Index), *Second.GetVertex(Index));
	for (uint32 Index = 0; Index < First.GetTriangleCount(); ++Index)
	{
		ASSERT_NE(First.GetTriangle(Index), nullptr);
		ASSERT_NE(Second.GetTriangle(Index), nullptr);
		EXPECT_EQ(First.GetTriangle(Index)->First, Second.GetTriangle(Index)->First);
		EXPECT_EQ(First.GetTriangle(Index)->Second, Second.GetTriangle(Index)->Second);
		EXPECT_EQ(First.GetTriangle(Index)->Third, Second.GetTriangle(Index)->Third);
	}

	const std::array<Durin::FVector3, 4> MeshVertices{
		Durin::FVector3{0.0, 0.0, 0.0}, Durin::FVector3{1.0, 0.0, 0.0},
		Durin::FVector3{0.0, 1.0, 0.0}, Durin::FVector3{2.0, 0.0, 0.0}};
	const std::array<uint32, 12> DirtyIndices{
		0, 1, 2, 2, 1, 0, 0, 1, 3, 0, 0, 1};
	Durin::FCollisionGeometryBuildDiagnostics MeshFacts;
	const Durin::FCollisionGeometryRef Mesh = Durin::FCollisionGeometryRef::BuildTriangleMesh(
		MeshVertices, DirtyIndices, &MeshFacts);
	ASSERT_TRUE(Mesh.IsValid());
	EXPECT_EQ(MeshFacts.SourceTriangles, 4u);
	EXPECT_EQ(MeshFacts.RetainedTriangles, 1u);
	EXPECT_EQ(MeshFacts.RemovedTriangles, 3u);
	EXPECT_EQ(Mesh.GetTriangle(0)->SourceOrdinal, 0u);
	EXPECT_EQ(Mesh.GetNodeCount(), 1u);
	EXPECT_TRUE(Mesh.GetNode(0)->IsLeaf());
	EXPECT_EQ(Mesh.GetNode(0)->GetLeafCount(), 1u);
	EXPECT_GE(MeshFacts.EstimatedPeakBytes, MeshFacts.RetainedBytes);
	Durin::FCollisionGeometryBuildDiagnostics RepeatFacts;
	const Durin::FCollisionGeometryRef Repeat = Durin::FCollisionGeometryRef::BuildTriangleMesh(
		MeshVertices, DirtyIndices, &RepeatFacts);
	ASSERT_TRUE(Repeat.IsValid());
	ASSERT_EQ(Mesh.GetNodeCount(), Repeat.GetNodeCount());
	ASSERT_EQ(Mesh.GetLeafTriangleCount(), Repeat.GetLeafTriangleCount());
	for (uint32 Index = 0; Index < Mesh.GetNodeCount(); ++Index)
	{
		EXPECT_EQ(Mesh.GetNode(Index)->Minimum, Repeat.GetNode(Index)->Minimum);
		EXPECT_EQ(Mesh.GetNode(Index)->Maximum, Repeat.GetNode(Index)->Maximum);
		EXPECT_EQ(Mesh.GetNode(Index)->First, Repeat.GetNode(Index)->First);
		EXPECT_EQ(Mesh.GetNode(Index)->CountOrSecond, Repeat.GetNode(Index)->CountOrSecond);
	}
	for (uint32 Index = 0; Index < Mesh.GetLeafTriangleCount(); ++Index)
		EXPECT_EQ(Mesh.GetLeafTriangle(Index), Repeat.GetLeafTriangle(Index));
}

TEST(FPhysicsCollisionGeometryStage2Tests, ReportsTransactionalBuilderFailures)
{
	Durin::FCollisionGeometryBuildDiagnostics Facts;
	const std::array<Durin::FVector3, 3> Collinear{
		Durin::FVector3{0.0}, Durin::FVector3{1.0, 0.0, 0.0}, Durin::FVector3{2.0, 0.0, 0.0}};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildConvexHull(Collinear, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::InvalidInput);
	std::array<Durin::FVector3, 257> Oversized{};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildConvexHull(Oversized, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::LimitExceeded);
	const std::array<uint32, 3> Triangle{0, 1, 2};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildTriangleMesh(Collinear, Triangle, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::EmptyAfterCleanup);
	const std::array<uint32, 3> Invalid{0, 1, 99};
	EXPECT_FALSE(Durin::FCollisionGeometryRef::BuildTriangleMesh(Collinear, Invalid, &Facts).IsValid());
	EXPECT_EQ(Facts.Status, Durin::ECollisionGeometryBuildStatus::InvalidInput);
}

TEST(FPhysicsCollisionGeometryStage2Tests, ProductionSweepAndOverlapMatchReferenceWithoutFallback)
{
	const std::array<Durin::FVector3, 4> Vertices{
		Durin::FVector3{0.0, -3.0, -3.0}, Durin::FVector3{0.0, 3.0, -3.0},
		Durin::FVector3{0.0, 3.0, 3.0}, Durin::FVector3{0.0, -3.0, 3.0}};
	const std::array<uint32, 6> Indices{0, 1, 2, 0, 2, 3};
	const Durin::FCollisionGeometryRef Mesh =
		Durin::FCollisionGeometryRef::BuildTriangleMesh(Vertices, Indices);
	ASSERT_TRUE(Mesh.IsValid());
	const std::array<Durin::FCollisionShape, 3> Queries{
		Durin::FCollisionShape::MakeBox({0.25, 0.25, 0.25}),
		Durin::FCollisionShape::MakeSphere(0.25),
		Durin::FCollisionShape::MakeCapsule(0.25, 0.5)};
	for (const Durin::FCollisionShape& Query : Queries)
	{
		Durin::FTransform Start;
		Start.Translation = {-2.0, 0.0, 0.0};
		Durin::FPhysicsQueryHit Reference;
		Durin::FPhysicsQueryHit Production;
		Durin::CollisionGeometry::FCollisionGeometryCounters Work;
		ASSERT_EQ(Durin::CollisionGeometry::Sweep(Query, Start, {4.0, 0.0, 0.0},
			Mesh, Durin::FTransform(), Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference,
			Reference), Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		ASSERT_EQ(Durin::CollisionGeometry::Sweep(Query, Start, {4.0, 0.0, 0.0},
			Mesh, Durin::FTransform(), Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production,
			Production, &Work), Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_NEAR(Reference.Time, Production.Time, 1.0e-10);
		EXPECT_EQ(Work.ReferenceFallbacks, 0u);
		EXPECT_GT(Work.AssetNodeTests, 0u);
		Start.Translation = {-0.1, 0.0, 0.0};
		ASSERT_EQ(Durin::CollisionGeometry::Overlap(Query, Start, Mesh, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Reference, Reference),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		ASSERT_EQ(Durin::CollisionGeometry::Overlap(Query, Start, Mesh, Durin::FTransform(),
			Durin::CollisionGeometry::ECollisionQueryAlgorithm::Production, Production, &Work),
			Durin::CollisionGeometry::ECollisionQueryStatus::Hit);
		EXPECT_NEAR(Reference.PenetrationDepth, Production.PenetrationDepth, 1.0e-10);
	}
}
