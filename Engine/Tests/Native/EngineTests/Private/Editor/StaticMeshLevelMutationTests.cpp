#include <gtest/gtest.h>
#include "Editor/EditorTransactionTestSupport.h"

#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "Operations/StaticMeshLevelMutationTestHooks.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "Engine/Level.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "StaticMeshLevelMutations.h"
#include "StaticMesh/StaticMesh.h"

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
				const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "LevelMutation";
				std::filesystem::create_directories(Root);
				Durin::Testing::RegisterMountPointForTests(
					"/LevelMutationTests/", Root.generic_string() + "/");
				return true;
			}();
			(void)bRegistered;
			Durin::FPackagePath Path;
			const bool bValidPath = Durin::FPackagePath::TryCreate(
				std::format("/LevelMutationTests/Level_{}", NextId++), Path);
			EXPECT_TRUE(bValidPath);
			if (!bValidPath) return;
			Package = Durin::NewObject<Durin::DPackage>(nullptr, Path.GetAssetName());
			Package->InitializeAssetPackage(Path);
			Level = Durin::NewObject<Durin::DLevel>(Package, Path.GetAssetName());
			EXPECT_EQ(Package->FindTopLevelAsset(Level->GetFName()), Level);
			Package->ClearDirty();
		}

		~FLevelFixture()
		{
			Durin::MarkObjectHierarchyAsGarbage(Package);
			Durin::CollectGarbage();
		}

		static inline uint64 NextId = 1;
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

TEST(FStaticMeshLevelMutationTests, AppliesOneAtomicBatchAndRestoresSavedRevision)
{
	FLevelFixture Fixture;
	Durin::Tests::FTestTransactorOwner Transactions;
	Transactions->EstablishSavedState(*Fixture.Package);
	Durin::FTransform SecondTransform;
	SecondTransform.Translation = {3.0, 4.0, 5.0};
	auto Request = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	Request.Description = "Create graybox pieces";
	Request.Mutations = {MakeCreate("Floor"), MakeCreate("Wall", SecondTransform)};

	const auto Plan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Request);
	ASSERT_TRUE(Plan) << Plan.Diagnostic.Message;
	ASSERT_TRUE(Plan.bHasChanges);
	const auto Result = Durin::Editor::Level::FStaticMeshLevelMutations::Execute(Plan, {Fixture.Level, Transactions.Get()});
	ASSERT_TRUE(Result) << Result.Diagnostic.Message;
	EXPECT_TRUE(Result.bChanged);
	EXPECT_NE(Fixture.Level->FindActorByName("Floor"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("Wall"), nullptr);
	EXPECT_TRUE(Fixture.Package->IsDirty());
	EXPECT_EQ(Transactions->GetUndoDescription(), "Create graybox pieces");

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Fixture.Level->FindActorByName("Floor"), nullptr);
	EXPECT_EQ(Fixture.Level->FindActorByName("Wall"), nullptr);
	EXPECT_FALSE(Fixture.Package->IsDirty());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_NE(Fixture.Level->FindActorByName("Floor"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("Wall"), nullptr);
	EXPECT_TRUE(Fixture.Package->IsDirty());
	Transactions->Reset();
}

TEST(FStaticMeshLevelMutationTests, UpdatesRenamesAndRemovesSupportedActors)
{
	FLevelFixture Fixture;
	auto* UpdateActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("UpdateMe");
	auto* RenameActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RenameMe");
	auto* RemoveActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RemoveMe");
	ASSERT_NE(UpdateActor, nullptr);
	ASSERT_NE(RenameActor, nullptr);
	ASSERT_NE(RemoveActor, nullptr);
	Durin::Tests::FTestTransactorOwner Transactions;
	Transactions->EstablishSavedState(*Fixture.Package);

	Durin::FTransform ChangedTransform;
	ChangedTransform.Translation = {8.0, 1.0, 2.0};
	auto Request = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	Request.Mutations = {
		{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Update, .TargetName = "UpdateMe", .Desired = {.Transform = ChangedTransform}},
		{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Rename, .TargetName = "RenameMe", .Desired = {.Name = "Renamed"}},
		{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Remove, .TargetName = "RemoveMe"},
	};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Request);
	ASSERT_TRUE(Plan) << Plan.Diagnostic.Message;
	ASSERT_TRUE(Durin::Editor::Level::FStaticMeshLevelMutations::Execute(Plan, {Fixture.Level, Transactions.Get()}));
	EXPECT_DOUBLE_EQ(UpdateActor->GetActorTransform().Translation.x, 8.0);
	EXPECT_EQ(Fixture.Level->FindActorByName("RenameMe"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("Renamed"), nullptr);
	EXPECT_EQ(Fixture.Level->FindActorByName("RemoveMe"), nullptr);

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_DOUBLE_EQ(UpdateActor->GetActorTransform().Translation.x, 0.0);
	EXPECT_NE(Fixture.Level->FindActorByName("RenameMe"), nullptr);
	EXPECT_EQ(Fixture.Level->FindActorByName("Renamed"), nullptr);
	EXPECT_NE(Fixture.Level->FindActorByName("RemoveMe"), nullptr);
	Transactions->Reset();
}

TEST(FStaticMeshLevelMutationTests, RejectsStalePlansWithoutMutation)
{
	FLevelFixture Fixture;
	auto Request = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	Request.Mutations = {MakeCreate("Planned")};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Request);
	ASSERT_TRUE(Plan);
	Fixture.Level->SpawnActor<Durin::ACameraActor>("ExternalChange");

	Durin::Tests::FTestTransactorOwner Transactions;
	const auto Result = Durin::Editor::Level::FStaticMeshLevelMutations::Execute(Plan, {Fixture.Level, Transactions.Get()});
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelMutationError::StaleTarget);
	EXPECT_EQ(Fixture.Level->FindActorByName("Planned"), nullptr);
	EXPECT_FALSE(Transactions->CanUndo());
}

TEST(FStaticMeshLevelMutationTests, SuppressesNoOpAndRejectsUnsupportedGraphs)
{
	FLevelFixture Fixture;
	auto* Actor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("Stable");
	ASSERT_NE(Actor, nullptr);
	Durin::Tests::FTestTransactorOwner Transactions;
	auto NoOp = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	NoOp.Mutations.push_back({
		.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Update,
		.TargetName = "Stable",
		.Desired = {.Transform = Actor->GetActorTransform()},
	});
	const auto NoOpPlan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(NoOp);
	ASSERT_TRUE(NoOpPlan);
	EXPECT_FALSE(NoOpPlan.bHasChanges);
	const auto NoOpResult = Durin::Editor::Level::FStaticMeshLevelMutations::Execute(NoOpPlan, {Fixture.Level, Transactions.Get()});
	EXPECT_TRUE(NoOpResult);
	EXPECT_FALSE(NoOpResult.bChanged);
	EXPECT_FALSE(Transactions->CanUndo());

	ASSERT_NE(Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "Unsupported"), nullptr);
	auto Remove = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	Remove.Mutations.push_back({.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Remove, .TargetName = "Stable"});
	const auto UnsupportedPlan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Remove);
	EXPECT_FALSE(UnsupportedPlan);
	EXPECT_EQ(UnsupportedPlan.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelMutationError::UnsupportedActor);
}

TEST(FStaticMeshLevelMutationTests, RejectsReadOnlyAndReplacedDocumentExecution)
{
	FLevelFixture Fixture;
	auto Request = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	Request.Mutations = {MakeCreate("Deferred")};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Request);
	ASSERT_TRUE(Plan);
	Durin::Tests::FTestTransactorOwner Transactions;

	const auto ReadOnly = Durin::Editor::Level::FStaticMeshLevelMutations::Execute(
		Plan, {.OpenLevel = Fixture.Level, .Transactions = Transactions.Get(), .bReadOnly = true});
	EXPECT_FALSE(ReadOnly);
	EXPECT_EQ(ReadOnly.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelMutationError::ReadOnly);
	FLevelFixture Replacement;
	const auto Replaced = Durin::Editor::Level::FStaticMeshLevelMutations::Execute(
		Plan, {.OpenLevel = Replacement.Level, .Transactions = Transactions.Get()});
	EXPECT_FALSE(Replaced);
	EXPECT_EQ(Replaced.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelMutationError::StaleTarget);
	EXPECT_EQ(Fixture.Level->FindActorByName("Deferred"), nullptr);
}

TEST(FStaticMeshLevelMutationTests, RedoRefusesNameCollisionWithoutChangingHistory)
{
	FLevelFixture Fixture;
	Durin::Tests::FTestTransactorOwner Transactions;
	auto Request = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	Request.Mutations = {MakeCreate("Managed")};
	const auto Plan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Request);
	ASSERT_TRUE(Plan);
	ASSERT_TRUE(Durin::Editor::Level::FStaticMeshLevelMutations::Execute(Plan, {Fixture.Level, Transactions.Get()}));
	ASSERT_TRUE(Transactions->Undo());
	auto* Collision = Fixture.Level->SpawnActor<Durin::ACameraActor>("Managed");
	ASSERT_NE(Collision, nullptr);

	EXPECT_FALSE(Transactions->Redo());
	EXPECT_TRUE(Transactions->CanRedo());
	EXPECT_EQ(Fixture.Level->FindActorByName("Managed"), Collision);
	Transactions->Reset();
}

TEST(FStaticMeshLevelMutationTests, RejectsUnavailableAssetBeforeMutation)
{
	FLevelFixture Fixture;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);
	Durin::MarkObjectHierarchyAsGarbage(Mesh);
	auto Request = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
	Request.Mutations.push_back({
		.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Create,
		.TargetName = "UnavailableMesh",
		.Desired = {.StaticMesh = Mesh},
	});

	const auto Plan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Request);
	EXPECT_FALSE(Plan);
	EXPECT_EQ(Plan.Diagnostic.Error, Durin::Editor::Level::EStaticMeshLevelMutationError::InvalidRequest);
	EXPECT_EQ(Fixture.Level->FindActorByName("UnavailableMesh"), nullptr);
}

TEST(FStaticMeshLevelMutationTests, RollsBackEveryInjectedLiveMutationFailure)
{
	using Durin::Editor::Level::Testing::EStaticMeshLevelMutationFailurePoint;
	const std::array FailurePoints = {
		EStaticMeshLevelMutationFailurePoint::AfterTemporaryRename,
		EStaticMeshLevelMutationFailurePoint::AfterRemove,
		EStaticMeshLevelMutationFailurePoint::AfterCreate,
		EStaticMeshLevelMutationFailurePoint::AfterFinalRename,
		EStaticMeshLevelMutationFailurePoint::AfterUpdate,
	};
	for (const EStaticMeshLevelMutationFailurePoint FailurePoint : FailurePoints)
	{
		FLevelFixture Fixture;
		auto* RenameActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RenameSource");
		auto* RemoveActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("RemoveSource");
		auto* UpdateActor = Fixture.Level->SpawnActor<Durin::AStaticMeshActor>("UpdateSource");
		ASSERT_NE(RenameActor, nullptr);
		ASSERT_NE(RemoveActor, nullptr);
		ASSERT_NE(UpdateActor, nullptr);
		Durin::Tests::FTestTransactorOwner Transactions;
		Transactions->EstablishSavedState(*Fixture.Package);
		Durin::FTransform ChangedTransform;
		ChangedTransform.Translation = {9.0, 8.0, 7.0};
		auto Request = Durin::Editor::Level::FStaticMeshLevelMutations::CaptureTarget(*Fixture.Level);
		Request.Mutations = {
			{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Rename, .TargetName = "RenameSource", .Desired = {.Name = "RenameDestination"}},
			{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Remove, .TargetName = "RemoveSource"},
			MakeCreate("CreatedDestination"),
			{.Kind = Durin::Editor::Level::EStaticMeshLevelMutationKind::Update, .TargetName = "UpdateSource", .Desired = {.Transform = ChangedTransform}},
		};
		const auto Plan = Durin::Editor::Level::FStaticMeshLevelMutations::Plan(Request);
		ASSERT_TRUE(Plan) << Plan.Diagnostic.Message;
		Durin::Editor::Level::Testing::SetStaticMeshLevelMutationFailurePoint(FailurePoint);

		const auto Result = Durin::Editor::Level::FStaticMeshLevelMutations::Execute(
			Plan, {Fixture.Level, Transactions.Get()});
		EXPECT_FALSE(Result);
		EXPECT_FALSE(Transactions->CanUndo());
		EXPECT_NE(Fixture.Level->FindActorByName("RenameSource"), nullptr);
		EXPECT_EQ(Fixture.Level->FindActorByName("RenameDestination"), nullptr);
		EXPECT_NE(Fixture.Level->FindActorByName("RemoveSource"), nullptr);
		EXPECT_EQ(Fixture.Level->FindActorByName("CreatedDestination"), nullptr);
		auto* RestoredUpdate = Durin::Cast<Durin::AStaticMeshActor>(
			Fixture.Level->FindActorByName("UpdateSource"));
		ASSERT_NE(RestoredUpdate, nullptr);
		EXPECT_DOUBLE_EQ(RestoredUpdate->GetActorTransform().Translation.x, 0.0);
		EXPECT_FALSE(Fixture.Package->IsDirty());
		Transactions->Reset();
	}
}
