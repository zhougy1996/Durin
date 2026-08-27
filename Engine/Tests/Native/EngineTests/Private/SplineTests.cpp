#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Actors/SplineMeshActor.h"
#include "AssetTools.h"
#include "CoreGlobals.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Editor/PropertyView.h"
#include "EngineTestSupport.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Misc/Paths.h"
#include "Math/Operations.h"
#include "NativeTestSupport.h"
#include "Spline/SplineCurve.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

#include <future>

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

		auto CurveTarget(Durin::DSplineComponent* Spline) const -> Durin::Editor::FPropertyEditTarget
		{
			return Durin::Editor::FPropertyEditTarget::ForMember(Spline, Curve);
		}

		auto PointsTarget(Durin::DSplineComponent* Spline) const -> Durin::Editor::FPropertyEditTarget
		{
			return CurveTarget(Spline).ForStructMember(Points);
		}

		auto PointFieldTarget(Durin::DSplineComponent* Spline, uint32 Index, Durin::FProperty* Field) const
			-> Durin::Editor::FPropertyEditTarget
		{
			return PointsTarget(Spline).ForArrayElement(Point, Index).ForStructMember(Field);
		}
	};
} // namespace

TEST(FSplineCurveTests, AuthoringOperationsMaintainStableUniquePointIds)
{
	Durin::FSplineCurve Curve;
	ASSERT_EQ(Curve.GetNumPoints(), 2u);
	const Durin::FGuid FirstId = Curve.GetPoint(0)->Id;
	const Durin::FGuid SecondId = Curve.GetPoint(1)->Id;
	EXPECT_TRUE(FirstId.IsValid());
	EXPECT_TRUE(SecondId.IsValid());
	EXPECT_NE(FirstId, SecondId);

	Durin::FSplinePoint Duplicate = *Curve.GetPoint(0);
	Durin::FSplinePoint Invalid = Duplicate;
	Invalid.Id.Invalidate();
	Curve.SetPoints({Invalid});
	ASSERT_TRUE(Curve.GetPoint(0)->Id.IsValid());

	Curve.SetPoints({Duplicate, Duplicate});
	ASSERT_EQ(Curve.GetNumPoints(), 2u);
	EXPECT_EQ(Curve.GetPoint(0)->Id, FirstId);
	EXPECT_NE(Curve.GetPoint(1)->Id, FirstId);
	const Durin::FGuid RepairedId = Curve.GetPoint(1)->Id;
	EXPECT_EQ(Curve.FindPointIndex(RepairedId), 1u);

	ASSERT_TRUE(Curve.MovePoint(1, 0));
	EXPECT_EQ(Curve.GetPoint(0)->Id, RepairedId);
	const uint32 AddedIndex = Curve.AddPoint(*Curve.GetPoint(0));
	EXPECT_NE(Curve.GetPoint(AddedIndex)->Id, RepairedId);
	ASSERT_TRUE(Curve.UpdatePoint(0, MakePoint({7.0, 8.0, 9.0})));
	EXPECT_EQ(Curve.GetPoint(0)->Id, RepairedId);
	const Durin::FGuid UpdatedId = Curve.GetPoint(0)->Id;
	ASSERT_TRUE(Curve.InsertPoint(1, *Curve.GetPoint(0)));
	EXPECT_NE(Curve.GetPoint(1)->Id, UpdatedId);
	const std::optional<uint32> DuplicatedIndex = Curve.DuplicatePoint(0);
	ASSERT_TRUE(DuplicatedIndex.has_value());
	EXPECT_NE(Curve.GetPoint(*DuplicatedIndex)->Id, UpdatedId);
	EXPECT_EQ(Curve.GetPoint(*DuplicatedIndex)->Position, Curve.GetPoint(0)->Position);
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

TEST(FSplineCurveTests, ManualHermiteAndChordLengthAutomaticTangentsMatchContracts)
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
	ExpectVectorNear(Evaluation->Evaluate({0, 1.0}).FirstDerivative, {3.909090909090909, 2.181818181818182, 0.0});
	ExpectVectorNear(Evaluation->Evaluate({1, 0.0}).FirstDerivative, {4.690909090909091, 2.618181818181818, 0.0});
}

TEST(FSplineCurveTests, ClampedAndAlignedTangentModesApplyTheirV2Rules)
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
	Durin::CollectGarbage();
}

TEST(FSplineComponentTests, DuplicateObjectPreservesIdsAndPublishesSnapshot)
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

	auto* Duplicate = Durin::DuplicateObject(Source, nullptr, "DuplicateSpline");
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetSplinePoints(), Source->GetSplinePoints());
	EXPECT_TRUE(Duplicate->IsClosedLoop());
	EXPECT_NEAR(Duplicate->GetLocalSplineLength(), Source->GetLocalSplineLength(), 1.e-8);
	ExpectVectorNear(Duplicate->GetSampleAtParameter({0, 0.35}).Position,
		Source->GetSampleAtParameter({0, 0.35}).Position);

	Durin::MarkAsGarbage(Source);
	Durin::MarkAsGarbage(Duplicate);
	Durin::CollectGarbage();
}

TEST(FSplineReflectionTests, RegistersV2ControlPointFieldsOnly)
{
	InitializeDObjectSystem();
	Durin::DStruct* PointStruct = Durin::FSplinePoint::StaticStruct();
	ASSERT_NE(PointStruct, nullptr);
	for (const char* PropertyName : {"Id", "Position", "ArriveTangent", "LeaveTangent", "OutgoingInterpolation", "TangentMode"})
		EXPECT_NE(PointStruct->FindPropertyByName(Durin::FName(PropertyName)), nullptr) << PropertyName;
	for (const char* RemovedName : {"Rotation", "Scale", "Type"})
		EXPECT_EQ(PointStruct->FindPropertyByName(Durin::FName(RemovedName)), nullptr) << RemovedName;
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

	Durin::Editor::FTransactionManager Transactions;
	const uint64 MountedContentRevision =
		Transactions.GetMountedContentMutationRevision();
	Durin::Editor::FPropertyView View;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	auto SubmitPosition = [&](const Durin::FVector3& Position, bool bContinuous) {
		const Durin::Editor::FPropertyEditTarget Target = Reflection.PointFieldTarget(Spline, 1, Reflection.Position);
		return View.SubmitPropertyValueEdit(Context, Target,
			[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
				*ScratchProperty->ContainerPtrToValuePtr<Durin::FVector3>(ScratchContainer, ScratchArrayIndex) = Position;
			}, bContinuous);
	};

	const Durin::FGuid EditedPointId = Spline->GetSplinePoint(1)->Id;
	ASSERT_TRUE(SubmitPosition({20.0, 0.0, 0.0}, true));
	ASSERT_TRUE(SubmitPosition({30.0, 0.0, 0.0}, true));
	View.FinishActiveEdit(&Context, false);
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(),
		MountedContentRevision);
	EXPECT_TRUE(Error.empty());
	EXPECT_NEAR(Spline->GetLocalSplineLength(), 30.0, 1.e-8);
	EXPECT_EQ(Spline->GetSplinePoint(1)->Id, EditedPointId);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(),
		MountedContentRevision);
	ExpectVectorNear(Spline->GetSplinePoint(1)->Position, {10.0, 0.0, 0.0});
	EXPECT_NEAR(Spline->GetLocalSplineLength(), 10.0, 1.e-8);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(),
		MountedContentRevision);
	ExpectVectorNear(Spline->GetSplinePoint(1)->Position, {30.0, 0.0, 0.0});

	Transactions.Clear();
	Durin::MarkAsGarbage(Spline);
	Durin::CollectGarbage();
}

TEST(FSplineMeshActorEditingTests, PreviewCancelUndoAndRedoReconcileWithoutIdentityChurn)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "SplineMeshActorEditing";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPointForTests(
			"/SplineMeshActorEditing/", Root.generic_string() + "/");
	}
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SplineMeshActorEditing/Transactions", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Level));
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

	Durin::Editor::FTransactionManager Transactions;
	Level->GetPackage()->ClearDirty();
	Transactions.EstablishSavedState(*Level->GetPackage());
	Durin::Editor::FPropertyView View;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
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
	EXPECT_NE(std::ranges::find(Segments, StableFirst), Segments.end());
	ASSERT_TRUE(View.FinishActiveEdit(&Context, true));
	ExpectVectorNear(Actor->GetSplineComponent()->GetSplinePoint(1)->Position, {100.0, 0.0, 0.0});
	EXPECT_FALSE(Level->GetPackage()->IsDirty());

	ASSERT_TRUE(SubmitPosition({130.0, 30.0, 0.0}, true));
	ASSERT_TRUE(View.FinishActiveEdit(&Context, false));
	EXPECT_TRUE(Transactions.CanUndo());
	ASSERT_TRUE(Transactions.Undo());
	ExpectVectorNear(Actor->GetSplineComponent()->GetSplinePoint(1)->Position, {100.0, 0.0, 0.0});
	Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	EXPECT_NE(std::ranges::find(Segments, StableFirst), Segments.end());
	ASSERT_TRUE(Transactions.Redo());
	ExpectVectorNear(Actor->GetSplineComponent()->GetSplinePoint(1)->Position, {130.0, 30.0, 0.0});
	Segments = Actor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	EXPECT_NE(std::ranges::find(Segments, StableFirst), Segments.end());
	EXPECT_TRUE(Error.empty());
	Transactions.Clear();
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Level->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FSplineComponentTests, LevelPackageRoundTripsV2ControlPointsAndIds)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "SplineV2Levels";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPointForTests("/SplineV2Tests/", Root.generic_string() + "/");
	}

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SplineV2Tests/RoundTrip", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Level));
	Durin::AActor* Actor = Level->SpawnActor<Durin::AActor>("SplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Path"));
	ASSERT_NE(Spline, nullptr);
	Level->GetPackage()->ClearDirty();
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	Spline->SetSplinePoints({
		MakePoint({1.0, 2.0, 3.0}, Durin::ESplineSegmentInterpolation::Linear),
		MakePoint({10.0, 12.0, 13.0}, Durin::ESplineSegmentInterpolation::Cubic, Durin::ESplineTangentMode::AutomaticClamped),
		MakePoint({20.0, 5.0, 6.0}, Durin::ESplineSegmentInterpolation::Cubic, Durin::ESplineTangentMode::ManualBroken),
	});
	EXPECT_TRUE(Level->GetPackage()->IsDirty());
	Spline->SetClosedLoop(true);
	const double SavedLength = Spline->GetLocalSplineLength();
	const Durin::FGuid SavedId = Spline->GetSplinePoint(1)->Id;
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	Durin::DObject* LoadedObject = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, LoadedObject));
	auto* LoadedLevel = Durin::Cast<Durin::DLevel>(LoadedObject);
	ASSERT_NE(LoadedLevel, nullptr);
	auto* LoadedSpline = LoadedLevel->FindActorByName("SplineActor")->FindComponentByClass<Durin::DSplineComponent>();
	ASSERT_NE(LoadedSpline, nullptr);
	EXPECT_EQ(LoadedSpline->GetNumSplinePoints(), 3u);
	EXPECT_TRUE(LoadedSpline->IsClosedLoop());
	EXPECT_NEAR(LoadedSpline->GetLocalSplineLength(), SavedLength, 1.e-8);
	EXPECT_EQ(LoadedSpline->GetSplinePoint(1)->Id, SavedId);
	EXPECT_EQ(LoadedSpline->GetSplinePoint(1)->TangentMode, Durin::ESplineTangentMode::AutomaticClamped);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}
