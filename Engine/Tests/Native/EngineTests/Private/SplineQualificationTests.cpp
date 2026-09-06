#include "Actors/SplineMeshActor.h"
#include "Collision/CollisionGeometry.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "Spline/SplineMeshDeformer.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
}

TEST(FSplineMeshCollisionQualificationTests, FrozenRoadStripMeetsSynchronousBuildAndRetentionBudgets)
{
	std::vector<FVector3> SourcePositions;
	std::vector<uint32> Indices;
	SourcePositions.reserve(256);
	Indices.reserve(254 * 3);
	for (uint32 Slice = 0; Slice < 128; ++Slice)
	{
		const double X = static_cast<double>(Slice) / 127.0;
		SourcePositions.push_back({X, -0.5, 0.0});
		SourcePositions.push_back({X, 0.5, 0.0});
		if (Slice == 0) continue;
		const uint32 Base = (Slice - 1) * 2;
		Indices.insert(Indices.end(), {Base, Base + 1, Base + 2,
			Base + 1, Base + 3, Base + 2});
	}
	ASSERT_EQ(SourcePositions.size(), 256u);
	ASSERT_EQ(Indices.size() / 3, 254u);
	FSplineMeshParams Params;
	Params.StartPosition = {0.0, 0.0, 0.0};
	Params.StartTangent = {0.7, 1.4, 0.3};
	Params.EndPosition = {1.0, 1.0, 0.5};
	Params.EndTangent = {1.2, -0.3, 0.7};
	Params.StartRollRadians = -0.25;
	Params.EndRollRadians = 0.6;
	Params.SourceForwardMin = 0.0;
	Params.SourceForwardMax = 1.0;
	std::vector<double> BuildMilliseconds;
	BuildMilliseconds.reserve(300);
	FCollisionGeometryRef LastGeometry;
	for (uint32 Iteration = 0; Iteration < 320; ++Iteration)
	{
		Params.EndPosition.z = 0.5 + static_cast<double>(Iteration % 7) * 0.001;
		const auto Start = std::chrono::steady_clock::now();
		std::vector<FVector3> Deformed;
		Deformed.reserve(SourcePositions.size());
		for (const FVector3& Position : SourcePositions)
			Deformed.push_back(FSplineMeshDeformer::DeformPosition(Params, Position));
		LastGeometry = FCollisionGeometryRef::BuildTriangleMesh(Deformed, Indices);
		const auto End = std::chrono::steady_clock::now();
		ASSERT_TRUE(LastGeometry.IsValid());
		if (Iteration >= 20)
			BuildMilliseconds.push_back(std::chrono::duration<double, std::milli>(End - Start).count());
	}
	std::ranges::sort(BuildMilliseconds);
	const double P95Milliseconds = BuildMilliseconds[284];
	const uint64 StressRetainedBytes = LastGeometry.GetRetainedBytes() * 128u;
	RecordProperty("collision_road_p95_ms", P95Milliseconds);
	RecordProperty("collision_128_segments_retained_bytes", StressRetainedBytes);
	EXPECT_LE(P95Milliseconds, 8.0);
	EXPECT_LE(StressRetainedBytes, 32ull * 1024ull * 1024ull);
}

TEST(FSplineMeshActorQualificationTests, FrozenThirtyTwoSegmentEditsMeetCpuAndStructuralMemoryBudgets)
{
	InitializeDObjectSystem();
	auto* World = NewObject<DWorld>(nullptr, "SplineMeshBudgetWorld");
	EXPECT_TRUE(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "SplineMeshBudgetLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<ASplineMeshActor>("SplineMeshActor");
	ASSERT_NE(Actor, nullptr);
	auto* Mesh = DStaticMesh::CreateDebugTriangle();
	Actor->SetPathMesh(Mesh);
	std::vector<FSplinePoint> Points;
	Points.reserve(33);
	for (uint32 Index = 0; Index < 33; ++Index)
		Points.emplace_back(FVector3(static_cast<double>(Index) * 100.0,
			std::sin(static_cast<double>(Index) * 0.35) * 25.0, 0.0));
	Actor->GetSplineComponent()->SetSplinePoints(Points);
	auto Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 32u);
	DActorComponent* StableFirst = Segments.front();
	std::vector<double> EditMilliseconds;
	EditMilliseconds.reserve(300);
	for (uint32 Iteration = 0; Iteration < 320; ++Iteration)
	{
		FSplinePoint Middle = *Actor->GetSplineComponent()->GetSplinePoint(16);
		Middle.Position.z = static_cast<double>((Iteration % 13) + 1) * 0.25;
		const auto Start = std::chrono::steady_clock::now();
		ASSERT_TRUE(Actor->GetSplineComponent()->UpdateSplinePoint(16, Middle));
		const auto End = std::chrono::steady_clock::now();
		if (Iteration >= 20)
			EditMilliseconds.push_back(std::chrono::duration<double, std::milli>(End - Start).count());
	}
	std::ranges::sort(EditMilliseconds);
	const double P95Milliseconds = EditMilliseconds[284];
	Segments = Actor->FindComponentsByClass<DSplineMeshComponent>();
	EXPECT_NE(std::ranges::find(Segments, StableFirst), Segments.end());
	for (DActorComponent* SegmentObject : Segments)
		EXPECT_EQ(Cast<DSplineMeshComponent>(SegmentObject)->GetStaticMesh(), Mesh);
	RecordProperty("reconstruction_32_segments_p95_ms", P95Milliseconds);
	RecordProperty("spline_mesh_component_structural_bytes", sizeof(DSplineMeshComponent));
	EXPECT_LE(P95Milliseconds, 4.0);
	EXPECT_LE(sizeof(DSplineMeshComponent), 4096u);
	MarkObjectHierarchyAsGarbage(World);
	MarkAsGarbage(Mesh);
	CollectGarbage();
}
