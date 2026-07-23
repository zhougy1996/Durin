#include "Components/SplineComponent.h"
#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorTransaction.h"
#include "Editor/ReflectedPropertyView.h"
#include "EngineTestSupport.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Misc/Paths.h"
#include "Spline/SplineCurve.h"

#include <gtest/gtest.h>

namespace
{
	auto ExpectVectorNear(const Durin::FVector3& Actual, const Durin::FVector3& Expected, double Tolerance = 1.e-8) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}

	auto MakePoint(const Durin::FVector3& Position, Durin::ESplinePointType Type = Durin::ESplinePointType::CurveAuto) -> Durin::FSplinePoint
	{
		Durin::FSplinePoint Point(Position);
		Point.Type = Type;
		return Point;
	}

	struct FSplineReflection
	{
		Durin::FStructProperty* Curve = nullptr;
		Durin::FArrayProperty* Points = nullptr;
		Durin::FProperty* Point = nullptr;
		Durin::FProperty* Position = nullptr;
		Durin::FProperty* ClosedLoop = nullptr;
		Durin::FProperty* ReparamSteps = nullptr;

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
			Result.ReparamSteps = CurveStruct->FindPropertyByName(Durin::FName("ReparamStepsPerSegment"));
			return Result;
		}

		auto GetCurve(Durin::DSplineComponent* Spline) const -> Durin::FSplineCurve*
		{
			return Curve->ContainerPtrToValuePtr<Durin::FSplineCurve>(Spline);
		}
		auto CurveTarget(Durin::DSplineComponent* Spline) const -> Durin::FReflectedPropertyEditTarget
		{
			return Durin::FReflectedPropertyEditTarget::ForMember(Spline, Curve);
		}
		auto PointsTarget(Durin::DSplineComponent* Spline) const -> Durin::FReflectedPropertyEditTarget
		{
			return CurveTarget(Spline).ForStructMember(Points);
		}
		auto PointFieldTarget(Durin::DSplineComponent* Spline, Durin::uint32 Index, Durin::FProperty* Field) const
			-> Durin::FReflectedPropertyEditTarget
		{
			void* PointContainer = Points->GetMutableElementPtr(GetCurve(Spline), Index);
			return PointsTarget(Spline).ForArrayElement(Point, Index).ForStructMember(Field);
		}
	};
} // namespace

TEST(FSplineCurveTests, HandlesEmptySinglePointAndDefaultCurve)
{
	Durin::FSplineCurve Curve;
	EXPECT_EQ(Curve.GetNumPoints(), 2u);
	EXPECT_EQ(Curve.GetNumSegments(), 1u);
	ExpectVectorNear(Curve.GetLocationAtParam(0.5), {50.0, 0.0, 0.0});
	EXPECT_NEAR(Curve.GetSplineLength(), 100.0, 1.e-8);

	Curve.ClearPoints();
	EXPECT_EQ(Curve.GetNumSegments(), 0u);
	EXPECT_DOUBLE_EQ(Curve.GetSplineLength(), 0.0);
	ExpectVectorNear(Curve.GetLocationAtParam(12.0), Durin::FVectorConstants::Zero);
	ExpectVectorNear(Curve.GetDirectionAtParam(12.0), Durin::FVectorConstants::Zero);

	Curve.AddPoint(MakePoint({4.0, 5.0, 6.0}));
	EXPECT_EQ(Curve.GetNumSegments(), 0u);
	ExpectVectorNear(Curve.GetLocationAtParam(-100.0), {4.0, 5.0, 6.0});
}

TEST(FSplineCurveTests, HermiteSegmentsHonorEndpointPositionsAndTangents)
{
	Durin::FSplinePoint Start = MakePoint({0.0, 0.0, 0.0}, Durin::ESplinePointType::Curve);
	Start.LeaveTangent = {12.0, 3.0, 0.0};
	Durin::FSplinePoint End = MakePoint({10.0, 10.0, 0.0}, Durin::ESplinePointType::Curve);
	End.ArriveTangent = {-2.0, 9.0, 0.0};

	Durin::FSplineCurve Curve;
	Curve.SetPoints({Start, End});
	ExpectVectorNear(Curve.GetLocationAtParam(0.0), Start.Position);
	ExpectVectorNear(Curve.GetLocationAtParam(1.0), End.Position);
	ExpectVectorNear(Curve.GetTangentAtParam(0.0), Start.LeaveTangent);
	ExpectVectorNear(Curve.GetTangentAtParam(1.0), End.ArriveTangent);
}

TEST(FSplineCurveTests, AutoTangentsAreContinuousAtInteriorPoints)
{
	Durin::FSplineCurve Curve;
	Curve.SetPoints({
		MakePoint({0.0, 0.0, 0.0}),
		MakePoint({1.0, 1.0, 0.0}),
		MakePoint({2.0, 0.0, 0.0}),
	});
	const Durin::FVector3 Incoming = Curve.GetTangentAtParam(1.0 - 1.e-7);
	const Durin::FVector3 Outgoing = Curve.GetTangentAtParam(1.0);
	ExpectVectorNear(Incoming, Outgoing, 1.e-5);
	ExpectVectorNear(Outgoing, {1.0, 0.0, 0.0}, 1.e-8);
}

TEST(FSplineCurveTests, ClosedLinearCurveWrapsAndIncludesClosingSegment)
{
	Durin::FSplineCurve Curve;
	Curve.SetPoints({
		MakePoint({0.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
		MakePoint({1.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
		MakePoint({1.0, 1.0, 0.0}, Durin::ESplinePointType::Linear),
		MakePoint({0.0, 1.0, 0.0}, Durin::ESplinePointType::Linear),
	});
	Curve.SetClosedLoop(true);
	EXPECT_EQ(Curve.GetNumSegments(), 4u);
	EXPECT_NEAR(Curve.GetSplineLength(), 4.0, 1.e-8);
	ExpectVectorNear(Curve.GetLocationAtParam(4.0), {0.0, 0.0, 0.0});
	ExpectVectorNear(Curve.GetLocationAtParam(-0.5), {0.0, 0.5, 0.0});
	ExpectVectorNear(Curve.GetLocationAtDistance(4.5), {0.5, 0.0, 0.0});
}

TEST(FSplineCurveTests, DistanceQueriesAreMonotonicAndAccurateForStraightSegments)
{
	Durin::FSplineCurve Curve;
	Curve.SetPoints({
		MakePoint({0.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
		MakePoint({10.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
	});
	Curve.SetReparamStepsPerSegment(4);
	EXPECT_NEAR(Curve.GetParamAtDistance(2.5), 0.25, 1.e-8);
	EXPECT_NEAR(Curve.GetDistanceAtParam(0.75), 7.5, 1.e-8);
	ExpectVectorNear(Curve.GetLocationAtDistance(7.5), {7.5, 0.0, 0.0});

	double PreviousParam = -1.0;
	for (int Step = 0; Step <= 20; ++Step)
	{
		const double Param = Curve.GetParamAtDistance(Curve.GetSplineLength() * Step / 20.0);
		EXPECT_GE(Param, PreviousParam);
		PreviousParam = Param;
	}
}

TEST(FSplineComponentTests, ConvertsSamplesToWorldSpaceWithoutChangingLocalDistance)
{
	InitializeDObjectSystem();
	auto* Spline = Durin::NewObject<Durin::DSplineComponent>(nullptr, "Spline");
	Spline->SetSplinePoints({
		MakePoint({0.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
		MakePoint({10.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
	});
	Durin::FTransform ComponentTransform;
	ComponentTransform.Translation = {1.0, 2.0, 3.0};
	ComponentTransform.Rotation = glm::angleAxis(glm::radians(90.0), Durin::FVectorConstants::Up);
	ComponentTransform.Scale3D = {2.0, 3.0, 4.0};
	Spline->SetWorldTransform(ComponentTransform);

	EXPECT_NEAR(Spline->GetSplineLength(), 10.0, 1.e-8);
	ExpectVectorNear(Spline->GetLocationAtParam(0.5, Durin::ESplineCoordinateSpace::World), {1.0, 12.0, 3.0}, 1.e-8);
	ExpectVectorNear(Spline->GetTangentAtParam(0.5, Durin::ESplineCoordinateSpace::World), {0.0, 20.0, 0.0}, 1.e-8);
	ExpectVectorNear(Spline->GetDirectionAtDistance(5.0, Durin::ESplineCoordinateSpace::World), {0.0, 1.0, 0.0}, 1.e-8);

	Durin::MarkAsGarbage(Spline);
	Durin::CollectGarbage();
}

TEST(FSplineComponentTests, DuplicateObjectGraphRestoresReflectedCurveDataAndCache)
{
	InitializeDObjectSystem();
	auto* Source = Durin::NewObject<Durin::DSplineComponent>(nullptr, "SourceSpline");
	Durin::FSplinePoint First = MakePoint({2.0, 3.0, 4.0}, Durin::ESplinePointType::Curve);
	First.LeaveTangent = {20.0, 5.0, 0.0};
	First.Scale = {2.0, 3.0, 4.0};
	Durin::FSplinePoint Second = MakePoint({15.0, 8.0, 4.0}, Durin::ESplinePointType::Curve);
	Second.ArriveTangent = {4.0, 12.0, 0.0};
	Source->SetSplinePoints({First, Second});
	Source->SetClosedLoop(true);
	Source->SetReparamStepsPerSegment(17);

	std::string Error;
	auto* Duplicate = Durin::Cast<Durin::DSplineComponent>(Durin::DuplicateObjectGraph(Source, nullptr, "DuplicateSpline", &Error));
	ASSERT_NE(Duplicate, nullptr) << Error;
	EXPECT_EQ(Duplicate->GetSplinePoints(), Source->GetSplinePoints());
	EXPECT_TRUE(Duplicate->IsClosedLoop());
	EXPECT_EQ(Duplicate->GetReparamStepsPerSegment(), 17);
	EXPECT_NEAR(Duplicate->GetSplineLength(), Source->GetSplineLength(), 1.e-8);
	ExpectVectorNear(Duplicate->GetLocationAtParam(0.35), Source->GetLocationAtParam(0.35));

	Durin::MarkAsGarbage(Source);
	Durin::MarkAsGarbage(Duplicate);
	Durin::CollectGarbage();
}

TEST(FSplineReflectionTests, RegistersAllControlPointValueFields)
{
	InitializeDObjectSystem();
	Durin::DStruct* PointStruct = Durin::FSplinePoint::StaticStruct();
	ASSERT_NE(PointStruct, nullptr);
	for (const char* PropertyName : {"Position", "ArriveTangent", "LeaveTangent", "Rotation", "Scale", "Type"})
	{
		EXPECT_NE(PointStruct->FindPropertyByName(Durin::FName(PropertyName)), nullptr) << PropertyName;
	}
}

TEST(FSplineEditingTests, SharedTransactionsPreserveSplineSetterSemanticsAndStablePaths)
{
	InitializeDObjectSystem();
	auto* Spline = Durin::NewObject<Durin::DSplineComponent>(nullptr, "TransactionalSpline");
	Spline->SetSplinePoints({
		MakePoint({0.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
		MakePoint({10.0, 0.0, 0.0}, Durin::ESplinePointType::Linear),
	});
	const FSplineReflection Reflection = FSplineReflection::Resolve(Spline);
	ASSERT_NE(Reflection.Curve, nullptr);
	ASSERT_NE(Reflection.Points, nullptr);
	ASSERT_NE(Reflection.Point, nullptr);
	ASSERT_NE(Reflection.Position, nullptr);
	ASSERT_NE(Reflection.ClosedLoop, nullptr);
	ASSERT_NE(Reflection.ReparamSteps, nullptr);

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView View;
	std::string Error;
	const Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	auto SubmitPosition = [&](const Durin::FVector3& Position, bool bContinuous) {
		const Durin::FReflectedPropertyEditTarget Target = Reflection.PointFieldTarget(Spline, 1, Reflection.Position);
		const Durin::FVector3 LiveValueBeforeProposal = Spline->GetSplinePoint(1)->Position;
		bool bLiveValueUntouched = false;
		const bool bSubmitted = View.SubmitPropertyValueEdit(Context, Target,
			[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, Durin::uint32 ScratchArrayIndex) {
				bLiveValueUntouched = Spline->GetSplinePoint(1)->Position == LiveValueBeforeProposal;
				*ScratchProperty->ContainerPtrToValuePtr<Durin::FVector3>(ScratchContainer, ScratchArrayIndex) = Position;
		}, bContinuous);
		EXPECT_TRUE(bLiveValueUntouched);
		return bSubmitted;
	};

	const Durin::FReflectedPropertyEditTarget PositionTarget = Reflection.PointFieldTarget(Spline, 1, Reflection.Position);
	ASSERT_EQ(PositionTarget.Path.size(), 4u);
	EXPECT_EQ(PositionTarget.Path[0].Property, Reflection.Curve);
	EXPECT_EQ(PositionTarget.Path[1].Property, Reflection.Points);
	EXPECT_EQ(PositionTarget.Path[1].Selector, Durin::EPropertyPathSelector::ArrayIndex);
	EXPECT_EQ(PositionTarget.Path[1].Index, 1u);
	EXPECT_EQ(PositionTarget.Path[2].Property, Reflection.Point);
	EXPECT_EQ(PositionTarget.Path[3].Property, Reflection.Position);

	ASSERT_TRUE(SubmitPosition({20.0, 0.0, 0.0}, true));
	ASSERT_TRUE(SubmitPosition({30.0, 0.0, 0.0}, true));
	EXPECT_NEAR(Spline->GetSplineLength(), 30.0, 1.e-8);
	View.FinishActiveEdit(&Context, false);
	EXPECT_TRUE(Error.empty());
	ASSERT_TRUE(Transactions.Undo());
	ExpectVectorNear(Spline->GetSplinePoint(1)->Position, {10.0, 0.0, 0.0});
	EXPECT_NEAR(Spline->GetSplineLength(), 10.0, 1.e-8);
	ASSERT_TRUE(Transactions.Redo());
	ExpectVectorNear(Spline->GetSplinePoint(1)->Position, {30.0, 0.0, 0.0});

	Transactions.Clear();
	ASSERT_TRUE(SubmitPosition({40.0, 0.0, 0.0}, true));
	View.FinishActiveEdit(&Context, true);
	ExpectVectorNear(Spline->GetSplinePoint(1)->Position, {30.0, 0.0, 0.0});
	EXPECT_FALSE(Transactions.CanUndo());

	auto SubmitCurveField = [&](Durin::FProperty* Field, auto Value) {
		const Durin::FReflectedPropertyEditTarget Target = Reflection.CurveTarget(Spline)
			.ForStructMember(Field);
		return View.SubmitPropertyValueEdit(Context, Target,
			[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, Durin::uint32 ScratchArrayIndex) {
				*ScratchProperty->ContainerPtrToValuePtr<std::decay_t<decltype(Value)>>(ScratchContainer, ScratchArrayIndex) = Value;
		}, false);
	};
	ASSERT_TRUE(SubmitCurveField(Reflection.ClosedLoop, true));
	EXPECT_TRUE(Spline->IsClosedLoop());
	ASSERT_TRUE(SubmitCurveField(Reflection.ReparamSteps, Durin::int32{4096}));
	EXPECT_EQ(Spline->GetReparamStepsPerSegment(), 1024);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Spline->GetReparamStepsPerSegment(), 10);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Spline->IsClosedLoop());

	Transactions.Clear();
	std::vector<Durin::FSplinePoint> AddedPoints = Spline->GetSplinePoints();
	AddedPoints.push_back(MakePoint({50.0, 0.0, 0.0}, Durin::ESplinePointType::Linear));
	Durin::FReflectedPropertyEditTarget PointsTarget = Reflection.PointsTarget(Spline);
	PointsTarget.Kind = Durin::EPropertyChangeKind::ArrayAdd;
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, PointsTarget,
		[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, Durin::uint32 ScratchArrayIndex) {
			auto* ScratchArray = static_cast<Durin::FArrayProperty*>(ScratchProperty);
			ScratchArray->Resize(ScratchContainer, AddedPoints.size(), ScratchArrayIndex);
			for (size_t Index = 0; Index < AddedPoints.size(); ++Index)
				*static_cast<Durin::FSplinePoint*>(ScratchArray->GetMutableElementPtr(ScratchContainer, Index, ScratchArrayIndex)) = AddedPoints[Index];
	}, false));
	EXPECT_EQ(Spline->GetNumSplinePoints(), 3u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Spline->GetNumSplinePoints(), 2u);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Spline->GetNumSplinePoints(), 3u);

	Transactions.Clear();
	Durin::MarkAsGarbage(Spline);
	Durin::CollectGarbage();
}

TEST(FSplineComponentTests, LevelPackageRoundTripsSplineControlPoints)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "SplineLevels";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/SplineTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SplineTests/RoundTrip", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Level));
	Durin::AActor* Actor = Level->SpawnActor<Durin::AActor>("SplineActor");
	ASSERT_NE(Actor, nullptr);
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Path"));
	ASSERT_NE(Spline, nullptr);
	Spline->SetSplinePoints({
		MakePoint({1.0, 2.0, 3.0}, Durin::ESplinePointType::Linear),
		MakePoint({10.0, 12.0, 13.0}, Durin::ESplinePointType::CurveAuto),
		MakePoint({20.0, 5.0, 6.0}, Durin::ESplinePointType::Curve),
	});
	Spline->SetClosedLoop(true);
	const double SavedLength = Spline->GetSplineLength();
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));

	Durin::DObject* LoadedObject = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(Path, LoadedObject));
	auto* LoadedLevel = Durin::Cast<Durin::DLevel>(LoadedObject);
	ASSERT_NE(LoadedLevel, nullptr);
	Durin::AActor* LoadedActor = LoadedLevel->FindActorByName("SplineActor");
	ASSERT_NE(LoadedActor, nullptr);
	auto* LoadedSpline = LoadedActor->FindComponentByClass<Durin::DSplineComponent>();
	ASSERT_NE(LoadedSpline, nullptr);
	EXPECT_EQ(LoadedSpline->GetNumSplinePoints(), 3u);
	EXPECT_TRUE(LoadedSpline->IsClosedLoop());
	EXPECT_NEAR(LoadedSpline->GetSplineLength(), SavedLength, 1.e-8);
	ExpectVectorNear(LoadedSpline->GetSplinePoint(1)->Position, {10.0, 12.0, 13.0});
	EXPECT_EQ(LoadedSpline->GetSplinePoint(2)->Type, Durin::ESplinePointType::Curve);
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}
