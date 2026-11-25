#include "Components/SplineComponent.h"
#include "Misc/UnifiedError.h"
#include "NativePackageUnloadTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Components/SplineMeshComponent.h"
#include "Actors/SplineMeshActor.h"
#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "CoreGlobals.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Transactions/Transaction.h"
#include "PropertyEditor/PropertyView.h"
#include "EngineTestSupport.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Math/Operations.h"
#include "NativeTestSupport.h"
#include "Spline/SplineCurve.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"


namespace
{
	auto ExpectVectorNear(const Durin::FVector3& Actual, const Durin::FVector3& Expected, double Tolerance = 1.e-8) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}

	auto MakePoint(const Durin::FVector3& Position,
		Durin::ESplineSegmentInterpolation Interpolation = Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode TangentMode = Durin::ESplineTangentMode::Automatic) -> Durin::FSplinePoint
	{
		Durin::FSplinePoint Point(Position);
		Point.OutgoingInterpolation = Interpolation;
		Point.TangentMode = TangentMode;
		return Point;
	}

	auto MakeAttributeCurve(const std::array<double, 3>& Keys, Durin::ESplineSegmentInterpolation Interpolation)
		-> Durin::FSplineCurve
	{
		using namespace Durin;
		const std::array<double, 3> RollDegrees{0.0, 40.0, 100.0};
		const std::array<double, 3> Scales{1.0, 2.0, 5.0};
		std::vector<FSplinePoint> Points;
		for (size_t Index = 0; Index < Keys.size(); ++Index)
		{
			auto Point = MakePoint({static_cast<double>(Index), 0.0, 0.0}, Interpolation);
			Point.InputKey = Keys[Index];
			Point.Rotation = Math::MakeQuaternionFromAxisAngleDegrees(RollDegrees[Index], FVector3(1.0, 0.0, 0.0));
			Point.Scale = {Scales[Index], 1.0, 1.0};
			Points.push_back(Point);
		}
		FSplineCurve Curve;
		Curve.SetPoints(std::move(Points));
		return Curve;
	}

	auto ExpectAttributes(const Durin::FSplineSample& Sample, double RollDegrees, double Scale) -> void
	{
		using namespace Durin;
		const auto ExpectedRotation = Math::MakeQuaternionFromAxisAngleDegrees(RollDegrees, FVector3(1.0, 0.0, 0.0));
		ExpectVectorNear(Sample.Rotation * FVectorConstants::Up, ExpectedRotation * FVectorConstants::Up, 1.e-7);
		ExpectVectorNear(Sample.Scale, {Scale, 1.0, 1.0});
	}

	struct FSplineReflection
	{
		Durin::FStructProperty* Curve = nullptr;
		Durin::FArrayProperty* Points = nullptr;
		Durin::FProperty* Point = nullptr;
		Durin::FProperty* Position = nullptr;
		Durin::FProperty* ClosedLoop = nullptr;

		static auto Resolve(Durin::DSplineComponent* Spline) -> FSplineReflection
		{
			FSplineReflection Result;
			auto* CurveProperty = Spline ? Spline->GetClass()->FindPropertyByName("SplineCurve") : nullptr;
			if (!CurveProperty || CurveProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Struct) return Result;
			Result.Curve = static_cast<Durin::FStructProperty*>(CurveProperty);
			Durin::DStruct* CurveStruct = Result.Curve->GetStruct();
			auto* PointsProperty = CurveStruct ? CurveStruct->FindPropertyByName(Durin::FName("Points")) : nullptr;
			if (!PointsProperty || PointsProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Array) return Result;
			Result.Points = static_cast<Durin::FArrayProperty*>(PointsProperty);
			Result.Point = Result.Points->GetInner();
			auto* PointProperty = Result.Point && Result.Point->GetKind() == Durin::DurinCodeGen::EPropertyGenFlags::Struct
				? static_cast<Durin::FStructProperty*>(Result.Point) : nullptr;
			Result.Position = PointProperty && PointProperty->GetStruct()
				? PointProperty->GetStruct()->FindPropertyByName(Durin::FName("Position")) : nullptr;
			Result.ClosedLoop = CurveStruct->FindPropertyByName(Durin::FName("bClosedLoop"));
			return Result;
		}

		auto GetCurve(Durin::DSplineComponent* Spline) const -> Durin::FSplineCurve*
		{
			return Curve->ContainerPtrToValuePtr<Durin::FSplineCurve>(Spline);
		}

		auto CurveTarget(Durin::DSplineComponent* Spline) const -> Durin::FPropertyEditTarget
		{
			return Durin::FPropertyEditTarget::ForMember(Spline, Curve);
		}

		auto PointsTarget(Durin::DSplineComponent* Spline) const -> Durin::FPropertyEditTarget
		{
			return CurveTarget(Spline).ForStructMember(Points);
		}

		auto PointFieldTarget(Durin::DSplineComponent* Spline, uint32 Index, Durin::FProperty* Field) const
			-> Durin::FPropertyEditTarget
		{
			return PointsTarget(Spline).ForArrayElement(Point, Index).ForStructMember(Field);
		}
	};
} // namespace

TEST(FSplineCurveTests, AuthoringOperationsAddressPointsByIndex)
{
	using namespace Durin;
	FSplineCurve Curve;
	ASSERT_TRUE(Curve.InsertPoint(1, FSplinePoint({2.0, 0.0, 0.0})));
	EXPECT_DOUBLE_EQ(Curve.GetPoint(1)->InputKey, 0.5);
	ASSERT_TRUE(Curve.MovePoint(1, 0));
	EXPECT_DOUBLE_EQ(Curve.GetPoint(0)->Position.x, 2.0);
	EXPECT_DOUBLE_EQ(Curve.GetPoint(0)->InputKey, 0.0);
	ASSERT_EQ(Curve.DuplicatePoint(0), 1u);
	EXPECT_EQ(Curve.GetPoint(0)->Position, Curve.GetPoint(1)->Position);
	EXPECT_GT(Curve.GetPoint(1)->InputKey, Curve.GetPoint(0)->InputKey);
	ASSERT_TRUE(Curve.RemovePoint(1));
	EXPECT_EQ(Curve.AddPoint(FSplinePoint({8.0, 0.0, 0.0})), 3u);
	EXPECT_DOUBLE_EQ(Curve.GetPoint(3)->InputKey, 2.0);
}

TEST(FSplineCurveTests, EvaluationHandlesEmptySinglePointAndDefaultCurve)
{
	Durin::FSplineCurve Curve;
	auto Evaluation = Curve.BuildEvaluationData();
	EXPECT_EQ(Evaluation->GetNumSegments(), 1u);
	ExpectVectorNear(Evaluation->Evaluate({0, 0.5}).Position, {2.5, 0.0, 0.0});
	EXPECT_NEAR(Evaluation->GetLocalLength(), 5.0, 1.e-8);

	Curve.ClearPoints();
	Evaluation = Curve.BuildEvaluationData();
	EXPECT_EQ(Evaluation->GetNumSegments(), 0u);
	EXPECT_DOUBLE_EQ(Evaluation->GetLocalLength(), 0.0);
	ExpectVectorNear(Evaluation->Evaluate({12, 0.7}).Position, Durin::FVectorConstants::Zero);
	ExpectVectorNear(Evaluation->Evaluate({12, 0.7}).Direction, Durin::FVectorConstants::Zero);

	Curve.AddPoint(MakePoint({4.0, 5.0, 6.0}));
	Evaluation = Curve.BuildEvaluationData();
	ExpectVectorNear(Evaluation->Evaluate({0, 0.5}).Position, {4.0, 5.0, 6.0});
}

TEST(FSplineCurveTests, ManualHermiteAndKeyBasedAutomaticTangentsMatchContracts)
{
	Durin::FSplinePoint Start = MakePoint({0.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualBroken);
	Start.LeaveTangent = {12.0, 3.0, 0.0};
	Durin::FSplinePoint End = MakePoint({10.0, 10.0, 0.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualBroken);
	End.ArriveTangent = {-2.0, 9.0, 0.0};
	Durin::FSplineCurve Curve;
	Curve.SetPoints({Start, End});
	auto Evaluation = Curve.BuildEvaluationData();
	ExpectVectorNear(Evaluation->Evaluate({0, 0.25}).Position, {3.34375, 1.5625, 0.0});
	ExpectVectorNear(Evaluation->Evaluate({0, 0.5}).FirstDerivative, {12.5, 12.0, 0.0});
	ExpectVectorNear(Evaluation->Evaluate({0, 0.75}).SecondDerivative, {-29.0, -6.0, 0.0});

	Curve.SetPoints({MakePoint({0.0, 0.0, 0.0}), MakePoint({3.0, 4.0, 0.0}), MakePoint({9.0, 4.0, 0.0})});
	Evaluation = Curve.BuildEvaluationData();
	ExpectVectorNear(Evaluation->Evaluate({0, 1.0}).FirstDerivative, {4.5, 2.0, 0.0});
	ExpectVectorNear(Evaluation->Evaluate({1, 0.0}).FirstDerivative, {4.5, 2.0, 0.0});
}

TEST(FSplineCurveTests, NonuniformInputKeysMatchHermiteAndScaledDistanceContracts)
{
	using namespace Durin;
	FSplinePoint Start = MakePoint({0.0, 0.0, 0.0}, ESplineSegmentInterpolation::Cubic, ESplineTangentMode::ManualBroken);
	FSplinePoint End = MakePoint({12.0, 0.0, 0.0}, ESplineSegmentInterpolation::Cubic, ESplineTangentMode::ManualBroken);
	Start.InputKey = 2.0;
	End.InputKey = 5.0;
	Start.LeaveTangent = End.ArriveTangent = {4.0, 0.0, 0.0};
	End.Scale = FVector3(3.0);
	FSplineCurve Curve;
	Curve.SetPoints({Start, End});
	const auto Evaluation = Curve.BuildEvaluationData();
	const auto Sample = Evaluation->EvaluateAtInputKey(3.5);
	ExpectVectorNear(Sample.Position, {6.0, 0.0, 0.0});
	ExpectVectorNear(Sample.FirstDerivative, {4.0, 0.0, 0.0});
	ExpectVectorNear(Sample.Scale, {2.0, 2.0, 2.0});
	ExpectVectorNear(Evaluation->Evaluate({0, 0.5}).FirstDerivative, {12.0, 0.0, 0.0});
	const auto Scaled = Evaluation->BuildScaledDistanceData({2.0, 3.0, 1.0});
	EXPECT_NEAR(Scaled->GetLocalLength(), 24.0, 1.e-8);
	EXPECT_NEAR(Scaled->GetInputKeyAtParameter(Scaled->GetParameterAtLocalDistance(12.0)), 3.5, 1.e-8);
	ASSERT_TRUE(Curve.SetClosedLoopAtPosition(9.0));
	EXPECT_DOUBLE_EQ(Curve.BuildEvaluationData()->GetInputKeyAtParameter({1, 1.0}), 9.0);

	Start.OutgoingInterpolation = ESplineSegmentInterpolation::Constant;
	Curve.SetClosedLoop(false);
	Curve.SetPoints({Start, End});
	const auto Stepped = Curve.BuildEvaluationData();
	ExpectVectorNear(Stepped->EvaluateAtInputKey(4.9).Position, Start.Position);
	ExpectVectorNear(Stepped->EvaluateAtInputKey(5.0).Position, End.Position);
	EXPECT_DOUBLE_EQ(Stepped->GetLocalDistanceAtParameter({0, 0.9}), 0.0);
	EXPECT_DOUBLE_EQ(Stepped->GetLocalLength(), 12.0);
}

TEST(FSplineCurveTests, RotationAndScaleUseIndependentAutomaticCurves)
{
	using namespace Durin;
	// Golden values use the UE 5.8 default backend's automatic controls and cubic interpolation.
	const auto Stepped = MakeAttributeCurve({0.0, 1.0, 2.0}, ESplineSegmentInterpolation::Constant).BuildEvaluationData();
	const auto Linear = MakeAttributeCurve({0.0, 1.0, 2.0}, ESplineSegmentInterpolation::Linear).BuildEvaluationData();
	const auto SteppedSample = Stepped->EvaluateAtInputKey(0.5);
	const auto LinearSample = Linear->EvaluateAtInputKey(0.5);
	ExpectVectorNear(SteppedSample.Position, {0.0, 0.0, 0.0});
	ExpectVectorNear(LinearSample.Position, {0.5, 0.0, 0.0});
	ExpectAttributes(SteppedSample, 9.1500897482162, 1.375);
	ExpectAttributes(LinearSample, 9.1500897482162, 1.375);
	ExpectAttributes(Stepped->EvaluateAtInputKey(1.0), 40.0, 2.0);
}

TEST(FSplineCurveTests, RotationAndScaleTangentsUseNonuniformInputKeys)
{
	using namespace Durin;
	const auto Evaluation = MakeAttributeCurve({2.0, 4.0, 7.0}, ESplineSegmentInterpolation::Cubic).BuildEvaluationData();
	ExpectAttributes(Evaluation->EvaluateAtInputKey(3.0), 8.265220949583949, 1.425);
	ExpectAttributes(Evaluation->Evaluate({1, 0.5}), 80.88309782141665, 3.425);
	ExpectAttributes(Evaluation->EvaluateAtInputKey(5.5), 80.88309782141665, 3.425);
	ExpectAttributes(Evaluation->EvaluateAtInputKey(4.0), 40.0, 2.0);
}

TEST(FSplineCurveTests, RotationAndScaleWrapNeighborsAcrossCustomLoopKey)
{
	using namespace Durin;
	auto Curve = MakeAttributeCurve({2.0, 4.0, 7.0}, ESplineSegmentInterpolation::Linear);
	ASSERT_TRUE(Curve.SetClosedLoopAtPosition(11.0));
	const auto Evaluation = Curve.BuildEvaluationData();
	ExpectAttributes(Evaluation->EvaluateAtInputKey(3.0), 9.113850810181166, 1.175);
	ExpectAttributes(Evaluation->EvaluateAtInputKey(9.0), 46.2678779684352, 3.1785714285714284);
	ExpectAttributes(Evaluation->EvaluateAtInputKey(11.0), 0.0, 1.0);
	ExpectAttributes(Evaluation->Evaluate({2, 1.0}), 0.0, 1.0);
}

TEST(FSplineCurveTests, ClampedAndAlignedTangentModesApplyTheirContracts)
{
	Durin::FSplineCurve Curve;
	Curve.SetPoints({
		MakePoint({0.0, 0.0, 0.0}),
		MakePoint({10.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Cubic,
			Durin::ESplineTangentMode::AutomaticClamped),
		MakePoint({9.0, 0.1, 0.0}),
	});
	auto Evaluation = Curve.BuildEvaluationData();
	ExpectVectorNear(Evaluation->Evaluate({0, 1.0}).FirstDerivative, {});
	ExpectVectorNear(Evaluation->Evaluate({1, 0.0}).FirstDerivative, {});

	Durin::FSplinePoint Start = MakePoint({0.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualAligned);
	Start.ArriveTangent = {6.0, 0.0, 0.0};
	Start.LeaveTangent = {0.0, 2.0, 0.0};
	Durin::FSplinePoint End = MakePoint({10.0, 10.0, 0.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualBroken);
	End.ArriveTangent = {3.0, 4.0, 0.0};
	Curve.SetPoints({Start, End});
	Evaluation = Curve.BuildEvaluationData();
	ExpectVectorNear(Evaluation->Evaluate({0, 0.0}).FirstDerivative, {0.0, 2.0, 0.0});
	ExpectVectorNear(Evaluation->Evaluate({0, 1.0}).FirstDerivative, {3.0, 4.0, 0.0});
}

TEST(FSplineCurveTests, ClosedLinearCurveWrapsAndPreservesFullLoopDistance)
{
	Durin::FSplineCurve Curve;
	Curve.SetPoints({
		MakePoint({0.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
		MakePoint({1.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
		MakePoint({1.0, 1.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
		MakePoint({0.0, 1.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
	});
	Curve.SetClosedLoop(true);
	const auto Evaluation = Curve.BuildEvaluationData();
	EXPECT_EQ(Evaluation->GetNumSegments(), 4u);
	EXPECT_NEAR(Evaluation->GetLocalLength(), 4.0, 1.e-8);
	ExpectVectorNear(Evaluation->Evaluate({3, 1.0}).Position, {0.0, 0.0, 0.0});
	EXPECT_NEAR(Evaluation->GetLocalDistanceAtParameter({3, 1.0}), 4.0, 1.e-8);
	ExpectVectorNear(Evaluation->EvaluateAtLocalDistance(4.5).Position, {0.5, 0.0, 0.0});
}

TEST(FSplineCurveTests, AdaptiveDistanceBoundsAndNearestQueriesAreStable)
{
	Durin::FSplinePoint Start = MakePoint({0.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualBroken);
	Start.LeaveTangent = {0.0, 1000.0, 0.0};
	Durin::FSplinePoint End = MakePoint({1.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualBroken);
	End.ArriveTangent = {0.0, -1000.0, 0.0};
	Durin::FSplineCurve Curve;
	Curve.SetPoints({Start, End});
	const auto Evaluation = Curve.BuildEvaluationData();
	EXPECT_NEAR(Evaluation->GetLocalLength(), 500.0078, 0.007);
	EXPECT_TRUE(Evaluation->GetLocalBounds().bIsValid);
	EXPECT_LE(Evaluation->GetLocalBounds().Min.x, 0.0);
	EXPECT_GE(Evaluation->GetLocalBounds().Max.y, 333.0);

	double PreviousDistance = -1.0;
	for (int Step = 0; Step <= 100; ++Step)
	{
		const Durin::FSplineParameter Parameter{0, Step / 100.0};
		const double Distance = Evaluation->GetLocalDistanceAtParameter(Parameter);
		EXPECT_GE(Distance, PreviousDistance);
		PreviousDistance = Distance;
	}

	Durin::FSplineCurve Line;
	Line.SetPoints({MakePoint({0.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
		MakePoint({10.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear)});
	const auto LineEvaluation = Line.BuildEvaluationData();
	EXPECT_NEAR(LineEvaluation->GetLocalDistanceAtParameter({0, 0.75}), 7.5, 1.e-8);
	EXPECT_NEAR(LineEvaluation->GetParameterAtLocalDistance(3.0).T, 0.3, 1.e-8);
	ExpectVectorNear(LineEvaluation->EvaluateAtLocalDistance(7.5).Position, {7.5, 0.0, 0.0});
	const Durin::FSplineParameter Nearest = LineEvaluation->FindNearestParameter({3.0, 5.0, 0.0});
	EXPECT_EQ(Nearest.SegmentIndex, 0u);
	EXPECT_NEAR(Nearest.T, 0.3, 1.e-6);
}

TEST(FSplineCurveTests, ImmutableSnapshotSupportsConcurrentReads)
{
	Durin::FSplineCurve Curve;
	Curve.SetPoints({MakePoint({0.0, 0.0, 0.0}), MakePoint({3.0, 4.0, 0.0}), MakePoint({9.0, 4.0, 0.0})});
	const auto Evaluation = Curve.BuildEvaluationData();
	std::vector<std::future<double>> Readers;
	for (int ReaderIndex = 0; ReaderIndex < 8; ++ReaderIndex)
	{
		Readers.push_back(std::async(std::launch::async, [Evaluation] {
			double Checksum = 0.0;
			for (int Iteration = 0; Iteration < 1000; ++Iteration)
			{
				const auto Sample = Evaluation->EvaluateAtLocalDistance(Evaluation->GetLocalLength() * Iteration / 999.0);
				Checksum += Sample.Position.x + Sample.Position.y + Sample.Direction.x;
			}
			return Checksum;
		}));
	}
	const double Expected = Readers.front().get();
	for (size_t Index = 1; Index < Readers.size(); ++Index) EXPECT_DOUBLE_EQ(Readers[Index].get(), Expected);
}

TEST(FSplineComponentTests, PublishesWorldSpaceSamplesAndRevisionFlags)
{
	InitializeDObjectSystem();
	auto* Spline = Durin::NewObject<Durin::DSplineComponent>(nullptr, "Spline");
	const uint64 InitialRevision = Spline->GetSplineRevision();
	Spline->SetSplinePoints({
		MakePoint({0.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
		MakePoint({10.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
	});
	EXPECT_EQ(Spline->GetSplineRevision(), InitialRevision + 1);
	Spline->SetSplinePoints(Spline->GetSplinePoints());
	EXPECT_EQ(Spline->GetSplineRevision(), InitialRevision + 1);
	EXPECT_TRUE(Durin::EnumHasAllFlags(Spline->GetLastSplineChangeFlags(),
		Durin::ESplineChangeFlags::Topology | Durin::ESplineChangeFlags::Geometry | Durin::ESplineChangeFlags::Build));

	Durin::FTransform ComponentTransform;
	ComponentTransform.Translation = {1.0, 2.0, 3.0};
	ComponentTransform.Rotation = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		90.0, Durin::FVectorConstants::Up);
	ComponentTransform.Scale3D = {2.0, 3.0, 4.0};
	Spline->SetWorldTransform(ComponentTransform);
	EXPECT_NEAR(Spline->GetLocalSplineLength(), 10.0, 1.e-8);
	const Durin::FSplineSample Sample = Spline->GetSampleAtParameter({0, 0.5}, Durin::ESplineCoordinateSpace::World);
	ExpectVectorNear(Sample.Position, {1.0, 12.0, 3.0});
	ExpectVectorNear(Sample.FirstDerivative, {0.0, 20.0, 0.0});
	ExpectVectorNear(Sample.Direction, {0.0, 1.0, 0.0});

	Durin::MarkAsGarbage(Spline);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FSplineComponentTests, RejectsInvalidBulkCandidatesWithoutChangingPublishedState)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("AtomicSplinePackages");
	Testing::RegisterMountPointForTests("/AtomicSplineTests/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AtomicSplineTests/Level", Path));
	DLevel* Level = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Level));
	auto* Actor = Level->SpawnActor<AActor>("SplineActor");
	auto* Spline = Cast<DSplineComponent>(Actor->AddInstanceComponent(DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Spline->SetFlags(EObjectFlags::Transactional);
	auto* Package = Level->GetPackage();
	FSplinePoint Start({0.0, 0.0, 0.0});
	FSplinePoint End({10.0, 0.0, 0.0});
	Start.InputKey = 2.0;
	End.InputKey = 5.0;
	ASSERT_TRUE(Spline->SetSplinePoints({Start, End}, 9.0));
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_FALSE(Package->IsDirty());
	const auto Before = Spline->GetSplinePoints();
	const auto Evaluation = Spline->GetEvaluationData();
	const uint64 Revision = Spline->GetSplineRevision();
	const uint64 Topology = Spline->GetSplineTopologyRevision();
	int Notifications = 0;
	const auto Listener = Spline->AddSplineMutationListener([&](auto, auto, auto) { ++Notifications; });

	Tests::FTestEditorTransactorOwner Transactions;
	{
		Durin::FScopedTransaction Scope("Invalid spline bulk edits");
		auto Invalid = End;
		Invalid.InputKey = Start.InputKey;
		EXPECT_FALSE(Spline->SetSplinePoints({Start, Invalid}, 10.0));
		Invalid.InputKey = std::numeric_limits<double>::infinity();
		EXPECT_FALSE(Spline->SetSplinePoints({Start, Invalid}, 10.0));
		Invalid = End;
		Invalid.Rotation.x = std::numeric_limits<double>::quiet_NaN();
		EXPECT_FALSE(Spline->SetSplinePoints({Start, Invalid}));
		Invalid = End;
		Invalid.Position.x = 20.0;
		EXPECT_FALSE(Spline->SetSplinePoints({Start, Invalid}, End.InputKey));
		Invalid.InputKey = 10.0;
		EXPECT_FALSE(Spline->SetSplinePoints({Start, Invalid}));
		EXPECT_FALSE(Spline->SetSplinePoints({}, 11.0));
	}
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_EQ(Spline->GetSplinePoints(), Before);
	EXPECT_TRUE(Spline->IsClosedLoop());
	EXPECT_DOUBLE_EQ(Spline->GetSplineCurve().GetLoopKey(), 9.0);
	EXPECT_EQ(Spline->GetEvaluationData(), Evaluation);
	EXPECT_EQ(Spline->GetSplineRevision(), Revision);
	EXPECT_EQ(Spline->GetSplineTopologyRevision(), Topology);
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Notifications, 0);

	{
		Durin::FScopedTransaction Scope("Valid spline bulk edit");
		ASSERT_TRUE(Spline->SetSplinePoints({Start, FSplinePoint({6.0, 0.0, 0.0}), End}, 11.0));
	}
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Spline->GetSplineRevision(), Revision + 1);
	EXPECT_EQ(Notifications, 1);
	EXPECT_DOUBLE_EQ(Spline->GetSplinePoint(1)->InputKey, 3.5);
	EXPECT_DOUBLE_EQ(Spline->GetSplineCurve().GetLoopKey(), 11.0);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Spline->GetSplinePoints(), Before);
	EXPECT_DOUBLE_EQ(Spline->GetSplineCurve().GetLoopKey(), 9.0);
	Spline->RemoveSplineMutationListener(Listener);
	Transactions->Reset();
	ASSERT_TRUE(SavePackage(Package));
	EXPECT_TRUE(UnloadPackage(Path));
}

TEST(FSplineEditingTests, PointEditUndoRestoresCurveAndRedoRestoresForeignReorder)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DSplineComponent> Spline(NewObject<DSplineComponent>(nullptr, "PointRangeHistorySpline", EObjectFlags::Transactional));
	Tests::FTestEditorTransactorOwner History;
	const auto Original = Spline->GetSplinePoints();
	auto Changed = Original[0];
	Changed.Position = Original[1].Position;
	Changed.LeaveTangent.y = 2.0;
	{
		Durin::FScopedTransaction Scope("Edit spline point");
		ASSERT_TRUE(Spline->UpdateSplinePoint(0, Changed));
	}
	{
		Durin::FScopedTransaction Scope("Reorder in the same history");
		ASSERT_TRUE(Spline->MoveSplinePoint(0, 1));
	}
	ASSERT_TRUE(History->Undo());
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Spline->GetSplinePoints(), Original);
	ASSERT_TRUE(History->Redo());

	Tests::FTestTransactorOwner OtherHistory;
	{
		Durin::Tests::FBufferRecording Scope(OtherHistory.Get(), {.Description = "Reorder in another history"});
		ASSERT_TRUE(Spline->MoveSplinePoint(0, 1));
	}
	const auto Reordered = Spline->GetSplinePoints();
	ASSERT_NE(Reordered, Original);
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Spline->GetSplinePoints(), Original);
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(Spline->GetSplinePoints(), Reordered);
}

TEST(FSplineEditingTests, WholeCurveUndoAndRedoAcrossUnrecordedReorder)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DSplineComponent> Spline(NewObject<DSplineComponent>(nullptr, "ReflectedPointHistorySpline", EObjectFlags::Transactional));
	Tests::FTestEditorTransactorOwner History;
	const auto Original = Spline->GetSplinePoints();
	auto Changed = Original;
	Changed[0].Position.y += 2.0;
	{
		Durin::FScopedTransaction Scope("Replace spline points");
		ASSERT_TRUE(Spline->SetSplinePoints(Changed));
	}
	ASSERT_TRUE(History->CanUndo());
	ASSERT_TRUE(Spline->MoveSplinePoint(0, 1));
	const auto Reordered = Spline->GetSplinePoints();
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Spline->GetSplinePoints(), Original);
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(Spline->GetSplinePoints(), Reordered);
}

TEST(FSplineComponentTests, DuplicateObjectPreservesPointsAndPublishesSnapshot)
{
	InitializeDObjectSystem();
	auto* Source = Durin::NewObject<Durin::DSplineComponent>(nullptr, "SourceSpline");
	Durin::FSplinePoint First = MakePoint({2.0, 3.0, 4.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualBroken);
	First.LeaveTangent = {20.0, 5.0, 0.0};
	Durin::FSplinePoint Second = MakePoint({15.0, 8.0, 4.0}, Durin::ESplineSegmentInterpolation::Cubic,
		Durin::ESplineTangentMode::ManualBroken);
	Second.ArriveTangent = {4.0, 12.0, 0.0};
	Source->SetSplinePoints({First, Second});
	Source->SetClosedLoop(true);

	auto* Duplicate = Durin::DuplicateObject(Source, nullptr, "DuplicateSpline").value();
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetSplinePoints(), Source->GetSplinePoints());
	EXPECT_TRUE(Duplicate->IsClosedLoop());
	EXPECT_NEAR(Duplicate->GetLocalSplineLength(), Source->GetLocalSplineLength(), 1.e-8);
	ExpectVectorNear(Duplicate->GetSampleAtParameter({0, 0.35}).Position,
		Source->GetSampleAtParameter({0, 0.35}).Position);

	Durin::MarkAsGarbage(Source);
	Durin::MarkAsGarbage(Duplicate);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FSplineEditingTests, SharedTransactionsPublishSnapshotsAndPreserveStablePaths)
{
	InitializeDObjectSystem();
	auto* Spline = Durin::NewObject<Durin::DSplineComponent>(nullptr, "TransactionalSpline");
	Spline->SetSplinePoints({MakePoint({0.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear),
		MakePoint({10.0, 0.0, 0.0}, Durin::ESplineSegmentInterpolation::Linear)});
	const FSplineReflection Reflection = FSplineReflection::Resolve(Spline);
	ASSERT_NE(Reflection.Curve, nullptr);
	ASSERT_NE(Reflection.Points, nullptr);
	ASSERT_NE(Reflection.Point, nullptr);
	ASSERT_NE(Reflection.Position, nullptr);
	ASSERT_NE(Reflection.ClosedLoop, nullptr);

	Durin::Tests::FTestEditorTransactorOwner Transactions;
	Durin::FPropertyView View;
	std::string Error;
	const Durin::FPropertyViewContext Context{
		.ReportPropertyError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	auto SubmitPosition = [&](const Durin::FVector3& Position, bool bContinuous) {
		const Durin::FPropertyEditTarget Target = Reflection.PointFieldTarget(Spline, 1, Reflection.Position);
		return View.SubmitPropertyValueEdit(Context, Target,
			[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
				*ScratchProperty->ContainerPtrToValuePtr<Durin::FVector3>(ScratchContainer, ScratchArrayIndex) = Position;
			}, bContinuous);
	};

	const uint64 TopologyRevision = Spline->GetSplineTopologyRevision();
	ASSERT_TRUE(SubmitPosition({20.0, 0.0, 0.0}, true));
	ASSERT_TRUE(SubmitPosition({30.0, 0.0, 0.0}, true));
	View.FinishActiveEdit(&Context, false);
	EXPECT_TRUE(Error.empty());
	EXPECT_NEAR(Spline->GetLocalSplineLength(), 30.0, 1.e-8);
	EXPECT_EQ(Spline->GetSplineTopologyRevision(), TopologyRevision);
	ASSERT_TRUE(Transactions->Undo());
	ExpectVectorNear(Spline->GetSplinePoint(1)->Position, {10.0, 0.0, 0.0});
	EXPECT_NEAR(Spline->GetLocalSplineLength(), 10.0, 1.e-8);
	ASSERT_TRUE(Transactions->Redo());
	ExpectVectorNear(Spline->GetSplinePoint(1)->Position, {30.0, 0.0, 0.0});

	Transactions->Reset();
	Durin::MarkAsGarbage(Spline);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FSplineEditingTests, StructuralUndoAndRedoRestoreOrderAndInvalidateOldIndices)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DSplineComponent> Spline(NewObject<DSplineComponent>(nullptr, "RangeSpline", EObjectFlags::Transactional));
	const auto Before = Spline->GetSplinePoints();
	{
		Durin::FScopedTransaction Scope("Spline point edits");
		ASSERT_TRUE(Spline->Modify());
		ASSERT_TRUE(Spline->InsertSplinePoint(1, FSplinePoint({100, 200, 0})));
		ASSERT_TRUE(Spline->RemoveSplinePoint(0));
		ASSERT_TRUE(Spline->MoveSplinePoint(0, 1));
	}
	const auto After = Spline->GetSplinePoints();
	const uint64 Topology = Spline->GetSplineTopologyRevision();
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Spline->GetSplinePoints(), Before);
	EXPECT_GT(Spline->GetSplineTopologyRevision(), Topology);
	EXPECT_EQ(Spline->GetEvaluationData()->GetNumSegments(), Spline->GetNumSplineSegments());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Spline->GetSplinePoints(), After);
	EXPECT_EQ(Spline->GetEvaluationData()->GetNumSegments(), Spline->GetNumSplineSegments());
}

TEST(FSplineMeshActorEditingTests, PreviewCancelUndoAndRedoRecreateSegmentsWithStableSourceKeys)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "SplineMeshActorEditing";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::Testing::RegisterMountPointForTests(
			"/SplineMeshActorEditing/", Root.generic_string() + "/");
	}
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SplineMeshActorEditing/Transactions", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Level));
	auto* Actor = Level->SpawnActor<Durin::ASplineMeshActor>("SplineMeshActor");
	ASSERT_NE(Actor, nullptr);
	Actor->SetPathMesh(Durin::DStaticMesh::CreateDebugTriangle(Level));
	Actor->GetSplineComponent()->SetSplinePoints({MakePoint({0.0, 0.0, 0.0}),
		MakePoint({100.0, 0.0, 0.0}), MakePoint({200.0, 0.0, 0.0})});
	auto Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	ASSERT_EQ(Segments.size(), 2u);
	Durin::DActorComponent* StableFirst = Segments.front();
	const FSplineReflection Reflection = FSplineReflection::Resolve(Actor->GetSplineComponent());
	ASSERT_NE(Reflection.Position, nullptr);

	Durin::Tests::FTestEditorTransactorOwner Transactions;
	Level->GetPackage()->ClearDirty();
	Durin::FPropertyView View;
	std::string Error;
	const Durin::FPropertyViewContext Context{
		.ReportPropertyError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	auto SubmitPosition = [&](const Durin::FVector3& Position, bool bContinuous) {
		return View.SubmitPropertyValueEdit(Context,
			Reflection.PointFieldTarget(Actor->GetSplineComponent(), 1, Reflection.Position),
			[&](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<Durin::FVector3>(Container, ArrayIndex) = Position;
			}, bContinuous);
	};

	ASSERT_TRUE(SubmitPosition({120.0, 25.0, 0.0}, true));
	Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	EXPECT_EQ(std::ranges::find(Segments, StableFirst), Segments.end());
	ASSERT_TRUE(View.FinishActiveEdit(&Context, true));
	ExpectVectorNear(Actor->GetSplineComponent()->GetSplinePoint(1)->Position, {100.0, 0.0, 0.0});
	EXPECT_FALSE(Level->GetPackage()->IsDirty());

	ASSERT_TRUE(SubmitPosition({130.0, 30.0, 0.0}, true));
	ASSERT_TRUE(View.FinishActiveEdit(&Context, false));
	EXPECT_TRUE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Undo());
	ExpectVectorNear(Actor->GetSplineComponent()->GetSplinePoint(1)->Position, {100.0, 0.0, 0.0});
	Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	EXPECT_EQ(std::ranges::find(Segments, StableFirst), Segments.end());
	ASSERT_TRUE(Transactions->Redo());
	ExpectVectorNear(Actor->GetSplineComponent()->GetSplinePoint(1)->Position, {130.0, 30.0, 0.0});
	Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	EXPECT_EQ(std::ranges::find(Segments, StableFirst), Segments.end());
	EXPECT_TRUE(Error.empty());
	Transactions->Reset();
	EXPECT_TRUE(Durin::UnloadPackage(Level->GetPackage(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FSplineComponentTests, LevelPackageRoundTripsControlPointsAndLoopKey)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "SplineLevels";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::Testing::RegisterMountPointForTests("/SplinePackageTests/", Root.generic_string() + "/");
	}

	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SplinePackageTests/RoundTrip", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Level));
	Durin::AActor* Actor = Level->SpawnActor<Durin::AActor>("SplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Path"));
	ASSERT_NE(Spline, nullptr);
	Level->GetPackage()->ClearDirty();
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	using namespace Durin;
	auto First = MakePoint({1.0, 2.0, 3.0}, ESplineSegmentInterpolation::Linear);
	First.InputKey = 2.0;
	auto Middle = MakePoint({10.0, 12.0, 13.0}, ESplineSegmentInterpolation::Cubic, ESplineTangentMode::ManualBroken);
	Middle.InputKey = 4.0;
	Middle.ArriveTangent = {-3.0, 1.0, 2.0};
	Middle.LeaveTangent = {6.0, 2.0, -1.0};
	Middle.Rotation = Math::MakeQuaternionFromAxisAngleDegrees(45.0, FVectorConstants::Forward);
	Middle.Scale = {2.0, 3.0, 4.0};
	auto Last = MakePoint({20.0, 5.0, 6.0}, ESplineSegmentInterpolation::Cubic, ESplineTangentMode::AutomaticClamped);
	Last.InputKey = 7.0;
	Spline->SetSplinePoints({First, Middle, Last});
	EXPECT_TRUE(Level->GetPackage()->IsDirty());
	ASSERT_TRUE(Spline->SetClosedLoopAtPosition(11.0));
	const double SavedLength = Spline->GetLocalSplineLength();
	const FSplineSample SavedSeamSample = Spline->GetSampleAtSplineInputKey(9.0);
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));

	Durin::DObject* LoadedObject = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = Durin::LoadObject<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		LoadedObject = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	auto* LoadedLevel = Durin::Cast<Durin::DLevel>(LoadedObject);
	ASSERT_NE(LoadedLevel, nullptr);
	auto* LoadedSpline = LoadedLevel->FindActorByName("SplineActor")->FindComponentByClass<Durin::DSplineComponent>();
	ASSERT_NE(LoadedSpline, nullptr);
	ASSERT_EQ(LoadedSpline->GetNumSplinePoints(), 3u);
	EXPECT_TRUE(LoadedSpline->IsClosedLoop());
	EXPECT_NEAR(LoadedSpline->GetLocalSplineLength(), SavedLength, 1.e-8);
	EXPECT_DOUBLE_EQ(LoadedSpline->GetSplineCurve().GetLoopKey(), 11.0);
	EXPECT_EQ(*LoadedSpline->GetSplinePoint(0), First);
	EXPECT_EQ(*LoadedSpline->GetSplinePoint(1), Middle);
	EXPECT_EQ(*LoadedSpline->GetSplinePoint(2), Last);
	const FSplineSample LoadedSeamSample = LoadedSpline->GetSampleAtSplineInputKey(9.0);
	ExpectVectorNear(LoadedSeamSample.Position, SavedSeamSample.Position);
	ExpectVectorNear(LoadedSeamSample.Scale, SavedSeamSample.Scale);
	EXPECT_TRUE(Math::AreRotationsEquivalent(LoadedSeamSample.Rotation, SavedSeamSample.Rotation));
	EXPECT_TRUE(Durin::UnloadPackage(Path));
}

TEST(FSplineCurveTests, DistanceTableResolvesNonuniformCollinearAndInflectedCubics)
{
	using namespace Durin;
	for (bool Inflected : {false, true})
	{
		FSplineCurve Curve;
		FSplinePoint A({0, 0, 0}), B({100, 0, 0});
		A.TangentMode = B.TangentMode = ESplineTangentMode::ManualBroken;
		A.LeaveTangent = Inflected ? FVector3(100, 200, 0) : FVector3(10, 0, 0);
		B.ArriveTangent = Inflected ? FVector3(100, 200, 0) : FVector3(180, 0, 0);
		Curve.SetPoints({A, B});
		const auto Data = Curve.BuildEvaluationData();
		double Integral = 0;
		constexpr int Steps = 10000;
		for (int I = 0; I <= Steps; ++I)
			Integral += (I == 0 || I == Steps ? 1 : (I % 2 ? 4 : 2))
				* Math::Length(Data->Evaluate({0, I / double(Steps)}).FirstDerivative) / (3 * Steps);
		EXPECT_NEAR(Data->GetLocalLength(), Integral, 1.e-4);
		if (!Inflected)
			for (int I = 0; I <= 100; ++I)
				EXPECT_NEAR(Data->EvaluateAtLocalDistance(I).Position.x, I, 1.e-4);
	}
}

TEST(FSplineMeshActorEditingTests, ActorModifyRestoresInputsAfterGeneratedComponentsRetire)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem();
	auto* Level = NewObject<DLevel>(nullptr, "GeneratedTransactionLevel");
	TStrongObjectPtr<DLevel> Root(Level);
	auto* Actor = Level->SpawnActor<ASplineMeshActor>("GeneratedTransactionActor");
	Actor->SetPathMesh(DStaticMesh::CreateDebugTriangle(Level));
	Actor->GetSplineComponent()->SetSplinePoints({MakePoint({0.0, 0.0, 0.0}), MakePoint({100.0, 0.0, 0.0}), MakePoint({200.0, 0.0, 0.0})});
	ASSERT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 2u);
	Tests::FTestEditorTransactorOwner Transactions;
	{
		Durin::FScopedTransaction Scope("test", "Edit actor construction input", nullptr);
		ASSERT_TRUE(Actor->Modify());
		Actor->GetSplineComponent()->SetSplinePoints({MakePoint({0.0, 0.0, 0.0}), MakePoint({200.0, 0.0, 0.0})});
	}
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Actor->GetSplineComponent()->GetNumSplinePoints(), 3u);
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 2u);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Actor->GetSplineComponent()->GetNumSplinePoints(), 2u);
	EXPECT_EQ(Actor->FindComponentsByClass<DSplineMeshComponent>().size(), 1u);
	Transactions->Reset(); MarkObjectHierarchyAsGarbage(Level); Root.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
}
