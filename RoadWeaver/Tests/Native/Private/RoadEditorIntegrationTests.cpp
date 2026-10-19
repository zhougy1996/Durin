#include "RoadNet/RoadNetActor.h"
#include "RoadNet/RoadNetBuilder.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Transactions/Transaction.h"
#include "Transactions/Transactor.h"
#include "Components/SplineMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/StrongObjectPtr.h"
#include "StaticMesh/StaticMesh.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::RoadNet;

	class FRoadEditorIntegrationTests : public testing::TestWithParam<bool>
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			Mounts.emplace();
			Testing::RegisterMountPointForTests("/RoadEditor/",
				Testing::CreateTestFixtureDirectory("RoadEditor").generic_string() + "/");
			const std::string PackageName = "Network" + FGuid::NewGuid().ToString();
			Package = NewObject<DPackage>(nullptr, FName(PackageName));
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/RoadEditor/" + PackageName, Path));
			Package->InitializeAssetPackage(Path);
			PackageRoot = TStrongObjectPtr<DObject>(Package);
			Asset = NewObject<DRoadNet>(Package, "Network");
			Draft = NewObject<DRoadNet>(Asset, "Draft");
			Transactor = NewObject<DTransBuffer>(nullptr, "RoadEditorHistory");
			TransactorRoot = TStrongObjectPtr<DObject>(Transactor);
			Property = Asset->GetClass()->FindPropertyByName(FName("Definition"));
			ASSERT_NE(Property, nullptr);

			FRoadNetBuilder Builder;
			FRoad Road;
			Road.StartNodeId = Builder.AddNode("Start", {0, 0, 0});
			Road.EndNodeId = Builder.AddNode("End", {40, 0, 0});
			Road.ReferenceLine.SetPoints({FSplinePoint({0, 0, 0}), FSplinePoint({40, 0, 0})});
			Road.LaneSections = {{.EndDistanceMeters = 40,
				.Lanes = {{.Index = 1}, {.Index = -1, .Direction = ELaneDirection::AgainstReferenceLine}}}};
			Builder.AddRoad(Road);
			Original = Builder.TakeDefinition();
			Original.Planet.Id = FGuid::NewGuid();
			Original.Planet.Center = {0, 0, -100};
			Original.Planet.RadiusMeters = 100;
			FRoadSurface Surface;
			Surface.Mode = GetParam() ? ERoadSurfaceMode::Sphere : ERoadSurfaceMode::Plane;
			Surface.Origin = GetParam() ? Original.Planet.Center : FVector3(0);
			Surface.RadiusMeters = 100;
			std::string Error;
			ASSERT_TRUE(FitRoadDefinition(Original, Surface, Error)) << Error;
			ASSERT_TRUE(Asset->SetDefinition(Original, Error)) << Error;
			Actor = NewObject<ARoadNetActor>(Package, "Preview");
			Actor->SetPreviewMesh(DStaticMesh::CreateDebugTriangle(Package));
			Actor->SetRoadNet(Asset);
			ASSERT_EQ(Actor->GetGenerationState(), "Ready") << Actor->GetDiagnostic();
			Transactor->EstablishSavedState(*Package);
		}

		auto TearDown() -> void override
		{
			if (Actor) Actor->BeginDestroy();
			if (Transactor) EXPECT_TRUE(Transactor->Reset());
			if (Package)
			{
				Package->SetStandaloneResidency(false);
				PackageRoot.Reset();
				MarkObjectHierarchyAsGarbage(Package);
				CollectGarbage();
			}
		}

		auto Capture(const FDefinition& Value) -> FPropertyValueSnapshot
		{
			*Property->ContainerPtrToValuePtr<FDefinition>(Draft) = Value;
			FPropertyValueSnapshot Snapshot;
			EXPECT_TRUE(CapturePropertyValue(Property, Draft, 0, Snapshot));
			return Snapshot;
		}

		auto AddRoad() const -> FDefinition
		{
			auto Changed = Original;
			auto Added = Original.Roads.front();
			Added.Id = FGuid::NewGuid();
			Added.Name = "Added road";
			auto Points = Added.ReferenceLine.GetPoints();
			for (auto& Point : Points) Point.Id = FGuid::NewGuid();
			Added.ReferenceLine.SetPoints(std::move(Points));
			for (auto& Lane : Added.LaneSections.front().Lanes) Lane.Id = FGuid::NewGuid();
			Changed.Roads.push_back(std::move(Added));
			return Changed;
		}

		auto ExpectPreview(const FDefinition& Expected) -> void
		{
			const auto Current = Capture(Asset->GetDefinition());
			EXPECT_EQ(Current, Capture(Expected));
			ASSERT_EQ(Actor->GetGenerationState(), "Ready") << Actor->GetDiagnostic();
			ASSERT_EQ(Actor->GetAlignments().size(), Expected.Roads.size());
			size_t Intervals = 0;
			for (size_t Index = 0; Index < Expected.Roads.size(); ++Index)
			{
				const auto& Alignment = Actor->GetAlignments()[Index];
				Intervals += Alignment->GetIntervals().size();
				FRoadSample Sample;
				std::string Error;
				ASSERT_TRUE(Alignment->Sample(0, Sample, Error)) << Error;
				EXPECT_EQ(Sample.Lanes.front().LaneId, Expected.Roads[Index].LaneSections.front().Lanes.front().Id);
			}
			const auto Components = Actor->FindComponentsByClass<DSplineMeshComponent>();
			ASSERT_EQ(Components.size(), Intervals);
			for (auto* Component : Components)
				EXPECT_TRUE(Component->GetDerivedState() && Component->GetDerivedState()->IsValid());
		}

		std::optional<Testing::FScopedMountRegistryFixture> Mounts;
		DPackage* Package = nullptr;
		DRoadNet* Asset = nullptr;
		DRoadNet* Draft = nullptr;
		ARoadNetActor* Actor = nullptr;
		DTransBuffer* Transactor = nullptr;
		FProperty* Property = nullptr;
		TStrongObjectPtr<DObject> PackageRoot;
		TStrongObjectPtr<DObject> TransactorRoot;
		FDefinition Original;
	};

	TEST_P(FRoadEditorIntegrationTests, StructuralPropertyCommitUndoRedoRestoresGraphPreviewAndSavedRevision)
	{
		const auto Changed = AddRoad();
		ASSERT_TRUE(ValidateDefinition(Changed));
		Editor::FPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(Editor::FPropertyEditTarget::ForMember(Asset, Property), "Add road", *Transactor));
		ASSERT_EQ(Session.Apply(Capture(Changed)).GetStatus(), Editor::EPropertyEditResult::Changed);
		ExpectPreview(Changed);
		ASSERT_EQ(Session.Commit().GetStatus(), Editor::EPropertyEditResult::Changed);
		EXPECT_EQ((Transactor->GetQueueLength() - Transactor->GetUndoCount()), 1u);
		EXPECT_TRUE(Package->IsDirty());
		ASSERT_TRUE(Transactor->Undo());
		ExpectPreview(Original);
		EXPECT_FALSE(Package->IsDirty());
		ASSERT_TRUE(Transactor->Redo());
		ExpectPreview(Changed);
		EXPECT_TRUE(Package->IsDirty());
	}

	TEST_P(FRoadEditorIntegrationTests, CancelRestoresStructuralPreviewWithoutCreatingHistory)
	{
		Editor::FPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(Editor::FPropertyEditTarget::ForMember(Asset, Property), "Cancel road addition", *Transactor));
		ASSERT_EQ(Session.Apply(Capture(AddRoad())).GetStatus(), Editor::EPropertyEditResult::Changed);
		ASSERT_EQ(Actor->GetAlignments().size(), 2u);
		ASSERT_EQ(Session.Cancel().GetStatus(), Editor::EPropertyEditResult::Changed);
		ExpectPreview(Original);
		// Native Modify marks dirty even when preview restoration or rejection leaves no history.
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_EQ(Transactor->GetQueueLength(), 0u);
	}

	TEST_P(FRoadEditorIntegrationTests, InvalidStructuralDraftIsAtomicAndLeavesNoHistory)
	{
		auto Invalid = AddRoad();
		Invalid.Roads.back().LaneSections.front().Lanes.front().Id = Original.Roads.front().LaneSections.front().Lanes.front().Id;
		const auto Before = Actor->GetAlignments().front();
		Editor::FPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(Editor::FPropertyEditTarget::ForMember(Asset, Property), "Reject duplicate lane", *Transactor));
		EXPECT_EQ(Session.Apply(Capture(Invalid)).GetStatus(), Editor::EPropertyEditResult::Failed);
		EXPECT_EQ(Session.Commit().GetStatus(), Editor::EPropertyEditResult::NoChange);
		ExpectPreview(Original);
		EXPECT_EQ(Actor->GetAlignments().front(), Before);
		// Native Modify marks dirty even when preview restoration or rejection leaves no history.
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_EQ(Transactor->GetQueueLength(), 0u);
	}

	TEST_P(FRoadEditorIntegrationTests, ScopedWholeObjectStructuralEditReplaysThroughRoadValidation)
	{
		const auto Changed = AddRoad();
		std::string Error;
		{
			Editor::FScopedTransaction Transaction(Transactor, {"road", "Add road using Modify"});
			ASSERT_TRUE(Asset->Modify());
			ASSERT_TRUE(Asset->SetDefinition(Changed, Error)) << Error;
		}
		ExpectPreview(Changed);
		ASSERT_TRUE(Transactor->Undo());
		ExpectPreview(Original);
		ASSERT_TRUE(Transactor->Redo());
		ExpectPreview(Changed);
	}

	INSTANTIATE_TEST_SUITE_P(Surfaces, FRoadEditorIntegrationTests, testing::Bool(),
		[](const testing::TestParamInfo<bool>& Info) { return Info.param ? "Sphere" : "Plane"; });
}
