#include "RoadNet/RoadNet.h"
#include "RoadNet/RoadNetBuilder.h"
#include "RoadNet/RoadSurface.h"
#include "Math/Operations.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include <gtest/gtest.h>

using namespace Durin;
using namespace Durin::RoadNet;

namespace
{
	auto MakeRoad() -> FDefinition
	{
		FRoadNetBuilder Builder;
		FRoad Road;
		Road.StartNodeId = Builder.AddNode("Start", {0, 0, 0});
		Road.EndNodeId = Builder.AddNode("End", {100, 0, 0});
		Road.ReferenceLine.SetPoints({FSplinePoint({0, 0, 0}), FSplinePoint({100, 0, 0})});
		FLaneSection Section;
		Section.EndDistanceMeters = 100;
		Section.Lanes.push_back({.Index = 1});
		Section.Lanes.push_back({.Index = -1, .Direction = ELaneDirection::AgainstReferenceLine});
		Road.LaneSections.push_back(Section);
		Builder.AddRoad(Road);
		return Builder.TakeDefinition();
	}
}

TEST(RoadGraphContract, TerminalOwnershipUsesTrafficAndSection)
{
	auto Definition = MakeRoad();
	std::string Error;
	ASSERT_TRUE(ValidateDefinition(Definition, Error)) << Error;
	const auto& Road = Definition.Roads.front();
	const auto Along = FindLaneOwnership(Definition, Road.LaneSections[0].Lanes[0].Id);
	const auto Against = FindLaneOwnership(Definition, Road.LaneSections[0].Lanes[1].Id);
	ASSERT_TRUE(Along);
	ASSERT_TRUE(Against);
	EXPECT_TRUE(IsTerminalLane(*Along, Road.EndNodeId, true));
	EXPECT_FALSE(IsTerminalLane(*Along, Road.StartNodeId, true));
	EXPECT_TRUE(IsTerminalLane(*Against, Road.StartNodeId, true));
	EXPECT_TRUE(IsTerminalLane(*Against, Road.EndNodeId, false));
}

TEST(RoadGraphContract, RejectsMalformedGeometryBeforeSplineSanitization)
{
	for (int Case = 0; Case < 7; ++Case)
	{
		auto Definition = MakeRoad();
		auto& Road = Definition.Roads[0];
		auto Points = Road.ReferenceLine.GetPoints();
		if (Case == 0) Points[0].ArriveTangent.x = std::numeric_limits<double>::quiet_NaN();
		if (Case == 1) Points[1].LeaveTangent.y = std::numeric_limits<double>::infinity();
		if (Case == 2) Points[0].OutgoingInterpolation = static_cast<ESplineSegmentInterpolation>(255);
		if (Case == 3) Points[0].TangentMode = static_cast<ESplineTangentMode>(255);
		if (Case == 4) Points[1].Position = Points[0].Position;
		if (Case == 5) Points[1].Position.x += 1;
		Road.ReferenceLine.SetPoints(Points);
		if (Case == 6) Road.ReferenceLine.SetClosedLoop(true);
		std::string Error;
		EXPECT_FALSE(ValidateDefinition(Definition, Error)) << Case;
		EXPECT_FALSE(Error.empty());
	}
}

TEST(RoadGraphContract, SectionContinuityIsExplicitAndDirected)
{
	auto Definition = MakeRoad();
	auto& Road = Definition.Roads[0];
	Road.LaneSections[0].EndDistanceMeters = 50;
	auto Second = Road.LaneSections[0];
	Second.StartDistanceMeters = 50;
	Second.EndDistanceMeters = 100;
	for (auto& Lane : Second.Lanes) Lane.Id = FGuid::NewGuid();
	Road.LaneSections.push_back(Second);
	std::string Error;
	EXPECT_FALSE(ValidateDefinition(Definition, Error));
	Road.SectionTransitions = {
		{Road.LaneSections[0].Lanes[0].Id, Second.Lanes[0].Id},
		{Second.Lanes[1].Id, Road.LaneSections[0].Lanes[1].Id}};
	ASSERT_TRUE(ValidateDefinition(Definition, Error)) << Error;
	EXPECT_FALSE(IsTerminalLane(*FindLaneOwnership(Definition, Road.LaneSections[0].Lanes[0].Id), Road.EndNodeId, true));
	Road.LaneSections[1].StartDistanceMeters += 1;
	EXPECT_FALSE(ValidateDefinition(Definition, Error));
}

TEST(RoadGraphContract, JunctionRejectsReversedAndDisconnectedLanes)
{
	auto Definition = MakeRoad();
	const auto& Road = Definition.Roads[0];
	FJunction Junction;
	Junction.Id = FGuid::NewGuid();
	Junction.NodeId = Road.EndNodeId;
	Junction.LaneConnections.push_back({.Id = FGuid::NewGuid(),
		.IncomingLaneId = Road.LaneSections[0].Lanes[0].Id,
		.OutgoingLaneId = Road.LaneSections[0].Lanes[1].Id});
	Definition.Junctions.push_back(Junction);
	std::string Error;
	ASSERT_TRUE(ValidateDefinition(Definition, Error)) << Error;
	Definition.Junctions[0].NodeId = Road.StartNodeId;
	EXPECT_FALSE(ValidateDefinition(Definition, Error));
	Definition.Nodes.push_back({.Id = FGuid::NewGuid(), .Position = {200, 0, 0}});
	Definition.Junctions[0].NodeId = Definition.Nodes.back().Id;
	EXPECT_FALSE(ValidateDefinition(Definition, Error));
}

TEST(RoadGraphContract, RejectedCandidatePreservesPublishedValueAndDoesNotNotify)
{
	auto* Asset = NewObject<DRoadNet>(nullptr, "RoadAtomicity");
	ASSERT_NE(Asset, nullptr);
	std::string Error;
	auto Definition = MakeRoad();
	ASSERT_TRUE(Asset->SetDefinition(Definition, Error)) << Error;
	bool Notified = false;
	const auto Listener = Asset->AddMutationListener([&] { Notified = true; });
	Definition.Nodes[0].Position.x += 5;
	EXPECT_FALSE(Asset->SetDefinition(Definition, Error));
	EXPECT_FALSE(Notified);
	Asset->RemoveMutationListener(Listener);
	EXPECT_EQ(Asset->GetNodes()[0].Position.x, 0);
}

TEST(RoadSurfaceContract, PlaneProjectionUsesFinalLengthAndRigidCoordinates)
{
	auto Definition = MakeRoad();
	auto& Road = Definition.Roads[0];
	Road.ReferenceLine.SetPoints({FSplinePoint({0, 0, 10}), FSplinePoint({100, 0, 40})});
	FRoadSurface Surface;
	Surface.Mode = ERoadSurfaceMode::Plane;
	Surface.ElevationMeters = 2;
	std::shared_ptr<const FRoadAlignment> Snapshot;
	std::string Error;
	Definition.Nodes.front().Position = Road.ReferenceLine.GetPoints().front().Position;
	Definition.Nodes.back().Position = Road.ReferenceLine.GetPoints().back().Position;
	Road.LaneSections.back().EndDistanceMeters = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
	ASSERT_TRUE(FitRoadDefinition(Definition, Surface, Error)) << Error;
	ASSERT_TRUE(FRoadAlignment::Build(Definition.Roads[0], Definition.Planet, Snapshot, Error)) << Error;
	EXPECT_NEAR(Snapshot->GetLengthMeters(), 100, 1.e-6);
	FRoadSample Sample;
	ASSERT_TRUE(Snapshot->Sample(50, Sample, Error));
	EXPECT_NEAR(Sample.Position.x, 50, 1.e-6);
	EXPECT_NEAR(Sample.Position.z, 2, 1.e-8);
	EXPECT_EQ(Sample.Lanes.size(), 2);
	FTransform Transform;
	Transform.Translation = {123, -47, 9};
	Transform.Rotation = Math::MakeQuaternionFromAxisAngleDegrees(37.0, FVector3(0, 0, 1));
	FRoadSample World;
	ASSERT_TRUE(Snapshot->SampleWorld(50, Transform, World, Error)) << Error;
	EXPECT_LT(Math::Length(World.Position - (Transform.Rotation * Sample.Position + Transform.Translation)), 1.e-8);
	Transform.Scale3D.x = 2;
	EXPECT_FALSE(Snapshot->SampleWorld(50, Transform, World, Error));
	EXPECT_FALSE(Snapshot->Sample(101, Sample, Error));
}

TEST(RoadSurfaceContract, SphereQuarterArcMeetsRadialAndFrameBudget)
{
	auto Definition = MakeRoad();
	auto& Road = Definition.Roads[0];
	FSplinePoint A({1000, 0, 0}), B({0, 1000, 0});
	A.OutgoingInterpolation = ESplineSegmentInterpolation::Linear;
	Road.ReferenceLine.SetPoints({A, B});
	Road.LaneSections[0].EndDistanceMeters = Math::HalfPi<double>() * 1000;
	FRoadSurface Surface;
	Surface.Mode = ERoadSurfaceMode::Sphere;
	std::shared_ptr<const FRoadAlignment> Snapshot;
	std::string Error;
	Definition.Nodes.front().Position = Road.ReferenceLine.GetPoints().front().Position;
	Definition.Nodes.back().Position = Road.ReferenceLine.GetPoints().back().Position;
	Road.LaneSections.back().EndDistanceMeters = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
	ASSERT_TRUE(FitRoadDefinition(Definition, Surface, Error)) << Error;
	ASSERT_TRUE(FRoadAlignment::Build(Definition.Roads[0], Definition.Planet, Snapshot, Error)) << Error;
	EXPECT_NEAR(Snapshot->GetLengthMeters(), Math::HalfPi<double>() * 1000, 1.e-3);
	for (int Index = 0; Index <= 100; ++Index)
	{
		FRoadSample Sample;
		ASSERT_TRUE(Snapshot->Sample(Snapshot->GetLengthMeters() * (Index / 100.0), Sample, Error)) << Error;
		EXPECT_NEAR(Math::Length(Sample.Position), 1000, 1.e-3);
		EXPECT_NEAR(Math::Dot(Sample.Frame.Up, Math::Normalize(Sample.Position)), 1, 1.e-8);
		EXPECT_NEAR(Math::Dot(Sample.Frame.Forward, Sample.Frame.Up), 0, 1.e-8);
		EXPECT_LT(Math::Length(Math::Cross(Sample.Frame.Forward, Sample.Frame.Side) - Sample.Frame.Up), 1.e-8);
	}
	const auto Previous = Snapshot;
	auto& FinalRoad = Definition.Roads[0];
	FinalRoad.LaneSections[0].EndDistanceMeters = 100;
	EXPECT_FALSE(FRoadAlignment::Build(FinalRoad, Definition.Planet, Snapshot, Error));
	EXPECT_EQ(Snapshot, Previous);
	FinalRoad.ReferenceLine.SetPoints({FSplinePoint({1000, 0, 0}), FSplinePoint({-1000, 0, 0})});
	EXPECT_FALSE(FRoadAlignment::Build(FinalRoad, Definition.Planet, Snapshot, Error));
	EXPECT_EQ(Snapshot, Previous);
}

TEST(RoadSurfaceContract, SphereSeamsPolesAndLongArcsUseTheSameDistanceContract)
{
	for (int Fixture = 0; Fixture < 3; ++Fixture)
	{
		auto Value = MakeRoad();
		auto& Road = Value.Roads[0];
		std::vector<FSplinePoint> Points;
		const int Count = Fixture == 2 ? 4 : 3;
		for (int Index = 0; Index < Count; ++Index)
		{
			const double Angle = Math::DegreesToRadians((Fixture == 0 ? 135.0 : 0.0) + 90.0 * Index);
			FVector3 Position = Fixture == 1 ? FVector3(std::cos(Angle), 0, std::sin(Angle))
				: FVector3(std::cos(Angle), std::sin(Angle), 0);
			FSplinePoint Point(Position * 1000.0);
			Point.OutgoingInterpolation = ESplineSegmentInterpolation::Linear;
			Points.push_back(Point);
		}
		Road.ReferenceLine.SetPoints(Points);
		Road.LaneSections[0].EndDistanceMeters = (Count - 1) * Math::HalfPi<double>() * 1000;
		FRoadSurface Surface;
		Surface.Mode = ERoadSurfaceMode::Sphere;
		std::shared_ptr<const FRoadAlignment> Snapshot;
		std::string Error;
		Value.Nodes.front().Position = Points.front().Position;
		Value.Nodes.back().Position = Points.back().Position;
		Road.LaneSections.back().EndDistanceMeters = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
		ASSERT_TRUE(FitRoadDefinition(Value, Surface, Error)) << Error;
		ASSERT_TRUE(FRoadAlignment::Build(Value.Roads[0], Value.Planet, Snapshot, Error)) << Fixture << ": " << Error;
		EXPECT_NEAR(Snapshot->GetLengthMeters(), Value.Roads[0].LaneSections[0].EndDistanceMeters, 2.e-3);
		FRoadSample Previous;
		for (int Index = 0; Index <= 300; ++Index)
		{
			FRoadSample Sample;
			ASSERT_TRUE(Snapshot->Sample(Snapshot->GetLengthMeters() * (Index / 300.0), Sample, Error));
			EXPECT_NEAR(Math::Length(Sample.Position), 1000, 1.e-3);
			if (Index > 0)
			{
				EXPECT_GT(Sample.DistanceMeters, Previous.DistanceMeters);
				EXPECT_GT(Math::Dot(Sample.Frame.Up, Previous.Frame.Up), 0.99);
			}
			Previous = Sample;
		}
	}
}

TEST(RoadSurfaceContract, NonuniformStraightCubicSamplesByMeters)
{
	auto Value = MakeRoad();
	auto& Road = Value.Roads[0];
	auto Points = Road.ReferenceLine.GetPoints();
	for (auto& Point : Points) Point.TangentMode = ESplineTangentMode::ManualBroken;
	Points[0].LeaveTangent = {10, 0, 0};
	Points[1].ArriveTangent = {180, 0, 0};
	Road.ReferenceLine.SetPoints(Points);
	std::shared_ptr<const FRoadAlignment> Snapshot;
	std::string Error;
	ASSERT_TRUE(FRoadAlignment::Build(Road, {}, Snapshot, Error)) << Error;
	for (int Index = 0; Index <= 100; ++Index)
	{
		FRoadSample Sample;
		ASSERT_TRUE(Snapshot->Sample(Index, Sample, Error));
		EXPECT_NEAR(Sample.Position.x, Index, 1.e-4);
	}
}

TEST(RoadSurfaceContract, FinalCurveQueriesAndPreviewAreIdenticalAfterEditingElevation)
{
	auto Value = MakeRoad();
	auto Points = Value.Roads[0].ReferenceLine.GetPoints();
	Points[0].TangentMode = Points[1].TangentMode = ESplineTangentMode::ManualBroken;
	Points[0].LeaveTangent = {60, 40, 80};
	Points[1].ArriveTangent = {70, -20, -40};
	Value.Roads[0].ReferenceLine.SetPoints(Points);
	const auto Evaluation = Value.Roads[0].ReferenceLine.BuildEvaluationData();
	Value.Roads[0].LaneSections[0].EndDistanceMeters = Evaluation->GetLocalLength();
	std::string Error;
	std::shared_ptr<const FRoadAlignment> Snapshot;
	ASSERT_TRUE(FRoadAlignment::Build(Value.Roads[0], Value.Planet, Snapshot, Error)) << Error;
	EXPECT_EQ(Snapshot->GetLengthMeters(), Evaluation->GetLocalLength());
	for (const auto& Interval : Snapshot->GetIntervals())
		for (int I = 0; I <= 10; ++I)
		{
			const double T = I / 10.0;
			const auto Expected = Evaluation->Evaluate({Interval.SegmentIndex, std::lerp(Interval.StartT, Interval.EndT, T)});
			const auto Mesh = FSplineMeshDeformer::Evaluate(Interval.Params, T);
			EXPECT_LT(Math::Length(Mesh.Position - Expected.Position), 1.e-9);
		}
	for (int I = 0; I <= 100; ++I)
	{
		const double Distance = Evaluation->GetLocalLength() * (I / 100.0);
		FRoadSample Sample;
		ASSERT_TRUE(Snapshot->Sample(Distance, Sample, Error));
		EXPECT_EQ(Sample.Position, Evaluation->EvaluateAtLocalDistance(Distance).Position);
	}
}

TEST(RoadSurfaceContract, ExplicitFitPreservesIdentityAndStationsAndRejectsAtomically)
{
	auto Value = MakeRoad();
	FRoadSurface Sphere;
	Sphere.Mode = ERoadSurfaceMode::Sphere;
	Sphere.Origin = {0, 0, -1000};
	const auto Before = Value;
	std::string Error;
	ASSERT_TRUE(FitRoadDefinition(Value, Sphere, Error)) << Error;
	EXPECT_EQ(Value.Roads[0].Id, Before.Roads[0].Id);
	EXPECT_EQ(Value.Roads[0].ReferenceLine.GetPoints().front().Id, Before.Roads[0].ReferenceLine.GetPoints().front().Id);
	EXPECT_EQ(Value.Roads[0].ReferenceLine.GetPoints().back().Id, Before.Roads[0].ReferenceLine.GetPoints().back().Id);
	EXPECT_EQ(Value.Roads[0].LaneSections[0].Lanes[0].Id, Before.Roads[0].LaneSections[0].Lanes[0].Id);
	EXPECT_EQ(Value.Roads[0].LaneSections.back().EndDistanceMeters, Value.Roads[0].ReferenceLine.BuildEvaluationData()->GetLocalLength());
	const auto FittedPoints = Value.Roads[0].ReferenceLine.GetPoints();
	Sphere.Origin = Value.Nodes[0].Position;
	EXPECT_FALSE(FitRoadDefinition(Value, Sphere, Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Value.Roads[0].ReferenceLine.GetPoints(), FittedPoints);
	EXPECT_EQ(Value.Nodes[0].Position, Before.Nodes[0].Position);
}

TEST(RoadGraphContract, ExplicitAssetFitRemapsMultipleSectionsAndKeepsPlanetFixed)
{
	auto Value = MakeRoad();
	auto& Road = Value.Roads[0];
	Road.LaneSections[0].EndDistanceMeters = 25;
	auto Second = Road.LaneSections[0];
	Second.StartDistanceMeters = 25;
	Second.EndDistanceMeters = 100;
	for (auto& Lane : Second.Lanes) Lane.Id = FGuid::NewGuid();
	Road.LaneSections.push_back(Second);
	Road.SectionTransitions = {{Road.LaneSections[0].Lanes[0].Id, Second.Lanes[0].Id},
		{Second.Lanes[1].Id, Road.LaneSections[0].Lanes[1].Id}};
	auto* Asset = NewObject<DRoadNet>(nullptr, "ExplicitRoadFit");
	std::string Error;
	ASSERT_TRUE(Asset->SetDefinition(Value, Error)) << Error;
	int Notifications = 0;
	const auto Listener = Asset->AddMutationListener([&] { ++Notifications; });
	FRoadSurface Sphere;
	Sphere.Mode = ERoadSurfaceMode::Sphere;
	Sphere.Origin = Value.Planet.Center;
	ASSERT_TRUE(Asset->FitToSurface(Sphere, Error)) << Error;
	EXPECT_EQ(Notifications, 1);
	const auto& Final = Asset->GetRoads()[0];
	const double Length = Final.ReferenceLine.BuildEvaluationData()->GetLocalLength();
	EXPECT_NEAR(Final.LaneSections[0].EndDistanceMeters, Length * 0.25, 1.e-9);
	EXPECT_EQ(Final.LaneSections[1].StartDistanceMeters, Final.LaneSections[0].EndDistanceMeters);
	EXPECT_EQ(Final.SectionTransitions[0].IncomingLaneId, Road.SectionTransitions[0].IncomingLaneId);
	EXPECT_EQ(Final.SectionTransitions[1].OutgoingLaneId, Road.SectionTransitions[1].OutgoingLaneId);
	const auto Points = Final.ReferenceLine.GetPoints();
	Sphere.RadiusMeters *= 2;
	EXPECT_FALSE(Asset->FitToSurface(Sphere, Error));
	EXPECT_EQ(Notifications, 1);
	EXPECT_EQ(Asset->GetRoads()[0].ReferenceLine.GetPoints(), Points);
	EXPECT_EQ(Asset->GetDefinition().Planet.RadiusMeters, 1000);
	Asset->RemoveMutationListener(Listener);
}
