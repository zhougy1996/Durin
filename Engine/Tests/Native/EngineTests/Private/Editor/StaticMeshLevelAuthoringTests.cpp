#include <gtest/gtest.h>

#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "Actors/TerrainActor.h"
#include "Components/TerrainComponent.h"
#include "Authoring/StaticMeshLevelAuthoringTestHooks.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Engine/Level.h"
#include "Misc/Paths.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "StaticMeshLevelAuthoring.h"
#include "TerrainLevelAuthoring.h"
#include "GrayboxSceneAuthoring.h"
#include "StaticMesh/StaticMesh.h"
#include "Terrain/TerrainHeightmap.h"

namespace
{
	struct FLevelFixture
	{
		Durin::DPackage* Package = nullptr;
		Durin::DLevel* Level = nullptr;

		FLevelFixture()
		{
			Durin::Testing::InitializeDObjectSystemForTests();
			static const bool bRegistered = [] {
				const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "LevelAuthoring";
				std::filesystem::create_directories(Root);
				Durin::PathUtilities::RegisterMountPointForTests(
					"/LevelAuthoringTests/", Root.generic_string() + "/");
				return true;
			}();
			(void)bRegistered;
			Durin::FAssetPath Path;
			const bool bValidPath = Durin::FAssetPath::TryCreate(
				std::format("/LevelAuthoringTests/Level_{}", NextId++), Path);
			EXPECT_TRUE(bValidPath);
			if (!bValidPath) return;
			Package = Durin::NewObject<Durin::DPackage>(nullptr, Path.GetAssetName());
			Package->InitializeAssetPackage(Path);
			Level = Durin::NewObject<Durin::DLevel>(Package, Path.GetAssetName());
			EXPECT_TRUE(Package->SetAsset(Level));
			Package->ClearDirty();
		}

		~FLevelFixture()
		{
			Durin::MarkObjectHierarchyAsGarbage(Package);
			Durin::CollectGarbage();
		}

		static inline Durin::uint64 NextId = 1;
	};

	auto MakeCreate(Durin::FName Name, const Durin::FTransform& Transform = {})
		-> Durin::Editor::Level::FStaticMeshLevelMutation
	{
		return {
			.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Create,
			.TargetName = Name,
			.Desired = {.Transform = Transform},
		};
	}
}

TEST(FTerrainLevelAuthoringTests, PlacesOneRevisionAtomicallyAndRestoresSavedState)
{
	FLevelFixture Fixture;
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(Fixture.Level, "GoldenHeightmap");
	const std::array<Durin::uint16, 6> Samples{0, 10'000, 20'000, 30'000, 65'535, 40'000};
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(3, 2, Samples, Error)) << Error;
	Durin::Editor::FTransactionManager Transactions;
	Transactions.EstablishSavedState(*Fixture.Package);
	auto Request = Durin::Editor::Level::FTerrainLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.ActorName = "GoldenTerrain";
	Request.Heightmap = Heightmap;
	Request.ExpectedHeightmapRevision = Heightmap->GetRevision();
	Request.SpacingX = 2.0;
	Request.SpacingY = 3.0;
	Request.HeightScale = -12.0;
	Request.HeightOffset = 7.0;
	Request.Transform.Translation = {4.0, 5.0, 6.0};
	const auto Plan = Durin::Editor::Level::FTerrainLevelAuthoringService::Plan(Request);
	ASSERT_TRUE(Plan) << Plan.Diagnostic.Message;
	const auto Result = Durin::Editor::Level::FTerrainLevelAuthoringService::Execute(
		Plan, {Fixture.Level, &Transactions});
	ASSERT_TRUE(Result) << Result.Diagnostic.Message;
	ASSERT_NE(Result.Actor.Get(), nullptr);
	EXPECT_EQ(Result.Actor->GetTerrainComponent()->GetHeightmap(), Heightmap);
	EXPECT_DOUBLE_EQ(Result.Actor->GetTerrainComponent()->GetSpacingX(), 2.0);
	EXPECT_EQ(Result.Actor->GetActorTransform().Translation, Durin::FVector3(4.0, 5.0, 6.0));
	EXPECT_TRUE(Fixture.Package->IsDirty());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Fixture.Level->FindActorByName("GoldenTerrain"), nullptr);
	EXPECT_FALSE(Fixture.Package->IsDirty());
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_NE(Fixture.Level->FindActorByName("GoldenTerrain"), nullptr);
	EXPECT_TRUE(Fixture.Package->IsDirty());
	Transactions.Clear();
}

TEST(FTerrainLevelAuthoringTests, RejectsStaleReadOnlyAndInvalidRequestsWithoutMutation)
{
	FLevelFixture Fixture;
	auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(Fixture.Level, "Heightmap");
	const std::array<Durin::uint16, 4> Samples{0, 1, 2, 3};
	std::string Error;
	ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, Samples, Error)) << Error;
	auto Request = Durin::Editor::Level::FTerrainLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.ActorName = "Terrain";
	Request.Heightmap = Heightmap;
	Request.ExpectedHeightmapRevision = Heightmap->GetRevision();
	Request.SpacingX = 0.0;
	EXPECT_EQ(Durin::Editor::Level::FTerrainLevelAuthoringService::Plan(Request).Diagnostic.Error,
		Durin::Editor::Level::ETerrainLevelAuthoringError::InvalidProperties);
	Request.SpacingX = 1.0;
	const auto Plan = Durin::Editor::Level::FTerrainLevelAuthoringService::Plan(Request);
	ASSERT_TRUE(Plan);
	Fixture.Level->SpawnActor<Durin::ATerrainActor>("Other");
	const auto Stale = Durin::Editor::Level::FTerrainLevelAuthoringService::Execute(
		Plan, {Fixture.Level, nullptr});
	EXPECT_EQ(Stale.Diagnostic.Error, Durin::Editor::Level::ETerrainLevelAuthoringError::StaleTarget);
	EXPECT_EQ(Fixture.Level->FindActorByName("Terrain"), nullptr);
	Request = Durin::Editor::Level::FTerrainLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.ActorName = "Terrain";
	Request.Heightmap = Heightmap;
	Request.bReadOnly = true;
	EXPECT_EQ(Durin::Editor::Level::FTerrainLevelAuthoringService::Plan(Request).Diagnostic.Error,
		Durin::Editor::Level::ETerrainLevelAuthoringError::ReadOnly);
}

TEST(FGrayboxOpenArenaTests, BuildsConnectedOpenTopFromActualBoxBounds)
{
	Durin::FBox Bounds;
	Bounds.AddPoint({-2.0, -3.0, -4.0});
	Bounds.AddPoint({2.0, 5.0, 6.0});
	Durin::Editor::Level::FGrayboxOpenArenaLayout Layout;
	std::string Error;
	ASSERT_TRUE(Durin::Editor::Level::BuildGrayboxOpenArenaLayout(
		{.Width = 20.0, .Depth = 12.0, .FloorThickness = 0.5,
		 .WallHeight = 4.0, .WallThickness = 0.5}, Bounds, Layout, Error))
		<< Error;
	ASSERT_EQ(Layout.Pieces.size(), 5);
	EXPECT_EQ(Layout.Pieces[0].Name, Durin::FName("Graybox_Floor"));
	EXPECT_EQ(Layout.Pieces[1].Name, Durin::FName("Graybox_WallNorth"));
	EXPECT_TRUE(std::ranges::none_of(Layout.Pieces, [](const auto& Piece) {
		return Piece.Name == Durin::FName("Graybox_Ceiling");
	}));

	const auto& Floor = Layout.Pieces[0].Transform;
	const auto& North = Layout.Pieces[1].Transform;
	const Durin::FVector3 LocalSize = Bounds.Max - Bounds.Min;
	EXPECT_NEAR(Floor.Scale3D.x * LocalSize.x, 21.0, 1e-9);
	EXPECT_NEAR(Floor.Scale3D.y * LocalSize.y, 13.0, 1e-9);
	EXPECT_NEAR(Floor.Scale3D.z * LocalSize.z, 0.5, 1e-9);
	EXPECT_NEAR(North.Scale3D.x * LocalSize.x, 21.0, 1e-9);
	EXPECT_NEAR(North.Scale3D.y * LocalSize.y, 0.5, 1e-9);
	EXPECT_NEAR(North.Scale3D.z * LocalSize.z, 4.05, 1e-9);
}

TEST(FGrayboxOpenArenaTests, AddsCeilingOnlyWhenExplicitlyRequested)
{
	Durin::FBox Bounds;
	Bounds.AddPoint({-0.5, -0.5, -0.5});
	Bounds.AddPoint({0.5, 0.5, 0.5});
	Durin::Editor::Level::FGrayboxOpenArenaLayout Layout;
	std::string Error;
	Durin::Editor::Level::FGrayboxOpenArenaParams Params;
	Params.bCeiling = true;
	ASSERT_TRUE(Durin::Editor::Level::BuildGrayboxOpenArenaLayout(Params, Bounds, Layout, Error));
	ASSERT_EQ(Layout.Pieces.size(), 6);
	EXPECT_EQ(Layout.Pieces.back().Name, Durin::FName("Graybox_Ceiling"));
}

TEST(FGrayboxOpenArenaTests, RejectsInvalidDimensionsAndDegenerateBounds)
{
	Durin::FBox Bounds;
	Bounds.AddPoint({-0.5, -0.5, -0.5});
	Bounds.AddPoint({0.5, 0.5, 0.5});
	Durin::Editor::Level::FGrayboxOpenArenaLayout Layout;
	std::string Error;
	Durin::Editor::Level::FGrayboxOpenArenaParams Params;
	Params.Width = 0.0;
	EXPECT_FALSE(Durin::Editor::Level::BuildGrayboxOpenArenaLayout(Params, Bounds, Layout, Error));
	Params.Width = 20.0;
	Durin::FBox Degenerate;
	Degenerate.AddPoint({0.0, 0.0, 0.0});
	Degenerate.AddPoint({0.0, 1.0, 1.0});
	EXPECT_FALSE(Durin::Editor::Level::BuildGrayboxOpenArenaLayout(
		Params, Degenerate, Layout, Error));
}

TEST(FStaticMeshLevelAuthoringTests, AppliesOneAtomicBatchAndRestoresSavedRevision)
{
	FLevelFixture Fixture;
	Durin::Editor::FTransactionManager Transactions;
	Transactions.EstablishSavedState(*Fixture.Package);
	Durin::FTransform SecondTransform;
	SecondTransform.Translation = {3.0, 4.0, 5.0};
	auto Request = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.Description = "Create graybox pieces";
	Request.Mutations = {MakeCreate("Floor"), MakeCreate("Wall", SecondTransform)};

	const auto Plan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Request);
	ASSERT_TRUE(Plan) << Plan.Diagnostic.Message;
	ASSERT_TRUE(Plan.bHasChanges);
	const auto Result = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(Plan, {Fixture.Level, &Transactions});
	ASSERT_TRUE(Result) << Result.Diagnostic.Message;
	EXPECT_TRUE(Result.bChanged);
	EXPECT_NE(Fixture.Level->FindActorByName("Floor"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("Wall"), nullptr);
	EXPECT_TRUE(Fixture.Package->IsDirty());
	EXPECT_EQ(Transactions.GetUndoDescription(), "Create graybox pieces");

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Fixture.Level->FindActorByName("Floor"), nullptr);
	EXPECT_EQ(Fixture.Level->FindActorByName("Wall"), nullptr);
	EXPECT_FALSE(Fixture.Package->IsDirty());
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_NE(Fixture.Level->FindActorByName("Floor"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("Wall"), nullptr);
	EXPECT_TRUE(Fixture.Package->IsDirty());
	Transactions.Clear();
}

TEST(FStaticMeshLevelAuthoringTests, UpdatesRenamesAndRemovesSupportedActors)
{
	FLevelFixture Fixture;
	auto* UpdateActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("UpdateMe");
	auto* RenameActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RenameMe");
	auto* RemoveActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RemoveMe");
	ASSERT_NE(UpdateActor, nullptr);
	ASSERT_NE(RenameActor, nullptr);
	ASSERT_NE(RemoveActor, nullptr);
	Durin::Editor::FTransactionManager Transactions;
	Transactions.EstablishSavedState(*Fixture.Package);

	Durin::FTransform ChangedTransform;
	ChangedTransform.Translation = {8.0, 1.0, 2.0};
	auto Request = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.Mutations = {
		{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Update, .TargetName = "UpdateMe", .Desired = {.Transform = ChangedTransform}},
		{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Rename, .TargetName = "RenameMe", .Desired = {.Name = "Renamed"}},
		{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Remove, .TargetName = "RemoveMe"},
	};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Request);
	ASSERT_TRUE(Plan) << Plan.Diagnostic.Message;
	ASSERT_TRUE(Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(Plan, {Fixture.Level, &Transactions}));
	EXPECT_DOUBLE_EQ(UpdateActor->GetActorTransform().Translation.x, 8.0);
	EXPECT_EQ(Fixture.Level->FindActorByName("RenameMe"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("Renamed"), nullptr);
	EXPECT_EQ(Fixture.Level->FindActorByName("RemoveMe"), nullptr);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_DOUBLE_EQ(UpdateActor->GetActorTransform().Translation.x, 0.0);
	EXPECT_NE(Fixture.Level->FindActorByName("RenameMe"), nullptr);
	EXPECT_EQ(Fixture.Level->FindActorByName("Renamed"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("RemoveMe"), nullptr);
	Transactions.Clear();
}

TEST(FStaticMeshLevelAuthoringTests, RejectsStalePlansWithoutMutation)
{
	FLevelFixture Fixture;
	auto Request = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.Mutations = {MakeCreate("Planned")};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Request);
	ASSERT_TRUE(Plan);
	Fixture.Level->SpawnActor<Durin::ACameraActor>("ExternalChange");

	Durin::Editor::FTransactionManager Transactions;
	const auto Result = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(Plan, {Fixture.Level, &Transactions});
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelAuthoringError::StaleTarget);
	EXPECT_EQ(Fixture.Level->FindActorByName("Planned"), nullptr);
	EXPECT_FALSE(Transactions.CanUndo());
}

TEST(FStaticMeshLevelAuthoringTests, SuppressesNoOpAndRejectsUnsupportedGraphs)
{
	FLevelFixture Fixture;
	auto* Actor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("Stable");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::FTransactionManager Transactions;
	auto NoOp = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	NoOp.Mutations.push_back({
		.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Update,
		.TargetName = "Stable",
		.Desired = {.Transform = Actor->GetActorTransform()},
	});
	const auto NoOpPlan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(NoOp);
	ASSERT_TRUE(NoOpPlan);
	EXPECT_FALSE(NoOpPlan.bHasChanges);
	const auto NoOpResult = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(NoOpPlan, {Fixture.Level, &Transactions});
	EXPECT_TRUE(NoOpResult);
	EXPECT_FALSE(NoOpResult.bChanged);
	EXPECT_FALSE(Transactions.CanUndo());

	ASSERT_NE(Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Unsupported"), nullptr);
	auto Remove = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Remove.Mutations.push_back({.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Remove, .TargetName = "Stable"});
	const auto UnsupportedPlan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Remove);
	EXPECT_FALSE(UnsupportedPlan);
	EXPECT_EQ(UnsupportedPlan.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelAuthoringError::UnsupportedActor);
}

TEST(FStaticMeshLevelAuthoringTests, RejectsReadOnlyAndReplacedDocumentExecution)
{
	FLevelFixture Fixture;
	auto Request = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.Mutations = {MakeCreate("Deferred")};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Request);
	ASSERT_TRUE(Plan);
	Durin::Editor::FTransactionManager Transactions;

	const auto ReadOnly = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(
		Plan, {.OpenLevel = Fixture.Level, .Transactions = &Transactions, .bReadOnly = true});
	EXPECT_FALSE(ReadOnly);
	EXPECT_EQ(ReadOnly.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelAuthoringError::ReadOnly);
	FLevelFixture Replacement;
	const auto Replaced = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(
		Plan, {.OpenLevel = Replacement.Level, .Transactions = &Transactions});
	EXPECT_FALSE(Replaced);
	EXPECT_EQ(Replaced.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelAuthoringError::StaleTarget);
	EXPECT_EQ(Fixture.Level->FindActorByName("Deferred"), nullptr);
}

TEST(FStaticMeshLevelAuthoringTests, RedoRefusesNameCollisionWithoutChangingHistory)
{
	FLevelFixture Fixture;
	Durin::Editor::FTransactionManager Transactions;
	auto Request = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.Mutations = {MakeCreate("Managed")};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Request);
	ASSERT_TRUE(Plan);
	ASSERT_TRUE(Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(Plan, {Fixture.Level, &Transactions}));
	ASSERT_TRUE(Transactions.Undo());
	auto* Collision = Fixture.Level->SpawnActor<Durin::ACameraActor>("Managed");
	ASSERT_NE(Collision, nullptr);

	EXPECT_FALSE(Transactions.Redo());
	EXPECT_TRUE(Transactions.CanRedo());
	EXPECT_EQ(Fixture.Level->FindActorByName("Managed"), Collision);
	Transactions.Clear();
}

TEST(FStaticMeshLevelAuthoringTests, RejectsUnavailableAssetBeforeMutation)
{
	FLevelFixture Fixture;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);
	Durin::MarkObjectHierarchyAsGarbage(Mesh);
	auto Request = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
	Request.Mutations.push_back({
		.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Create,
		.TargetName = "UnavailableMesh",
		.Desired = {.StaticMesh = Mesh},
	});

	const auto Plan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Request);
	EXPECT_FALSE(Plan);
	EXPECT_EQ(Plan.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelAuthoringError::InvalidRequest);
	EXPECT_EQ(Fixture.Level->FindActorByName("UnavailableMesh"), nullptr);
}

TEST(FStaticMeshLevelAuthoringTests, RollsBackEveryInjectedLiveMutationFailure)
{
	using Durin::Editor::Level::Testing::EStaticMeshLevelAuthoringFailurePoint;
	const std::array FailurePoints = {
		EStaticMeshLevelAuthoringFailurePoint::AfterTemporaryRename,
		EStaticMeshLevelAuthoringFailurePoint::AfterRemove,
		EStaticMeshLevelAuthoringFailurePoint::AfterCreate,
		EStaticMeshLevelAuthoringFailurePoint::AfterFinalRename,
		EStaticMeshLevelAuthoringFailurePoint::AfterUpdate,
	};
	for (const EStaticMeshLevelAuthoringFailurePoint FailurePoint : FailurePoints)
	{
		FLevelFixture Fixture;
		auto* RenameActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RenameSource");
		auto* RemoveActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RemoveSource");
		auto* UpdateActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("UpdateSource");
		ASSERT_NE(RenameActor, nullptr);
		ASSERT_NE(RemoveActor, nullptr);
		ASSERT_NE(UpdateActor, nullptr);
		Durin::Editor::FTransactionManager Transactions;
		Transactions.EstablishSavedState(*Fixture.Package);
		Durin::FTransform ChangedTransform;
		ChangedTransform.Translation = {9.0, 8.0, 7.0};
		auto Request = Durin::Editor::Level::FStaticMeshLevelAuthoringService::CaptureTarget(*Fixture.Level);
		Request.Mutations = {
			{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Rename, .TargetName = "RenameSource", .Desired = {.Name = "RenameDestination"}},
			{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Remove, .TargetName = "RemoveSource"},
			MakeCreate("CreatedDestination"),
			{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Update, .TargetName = "UpdateSource", .Desired = {.Transform = ChangedTransform}},
		};
		const auto Plan = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Plan(Request);
		ASSERT_TRUE(Plan) << Plan.Diagnostic.Message;
		Durin::Editor::Level::Testing::SetStaticMeshLevelAuthoringFailurePoint(FailurePoint);

		const auto Result = Durin::Editor::Level::FStaticMeshLevelAuthoringService::Execute(
			Plan, {Fixture.Level, &Transactions});
		EXPECT_FALSE(Result);
		EXPECT_FALSE(Transactions.CanUndo());
		EXPECT_NE(Fixture.Level->FindActorByName("RenameSource"), nullptr);
		EXPECT_EQ(Fixture.Level->FindActorByName("RenameDestination"), nullptr);
		EXPECT_NE(Fixture.Level->FindActorByName("RemoveSource"), nullptr);
		EXPECT_EQ(Fixture.Level->FindActorByName("CreatedDestination"), nullptr);
		auto* RestoredUpdate = Durin::Cast<Durin::AStaticMeshActor>(
			Fixture.Level->FindActorByName("UpdateSource"));
		ASSERT_NE(RestoredUpdate, nullptr);
		EXPECT_DOUBLE_EQ(RestoredUpdate->GetActorTransform().Translation.x, 0.0);
		EXPECT_FALSE(Fixture.Package->IsDirty());
		Transactions.Clear();
	}
}
