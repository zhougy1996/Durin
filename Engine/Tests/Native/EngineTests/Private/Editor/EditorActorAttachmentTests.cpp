#include <gtest/gtest.h>
#include "Editor/EditorTransactionTestSupport.h"

#include "Actors/CameraActor.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Transactions/Transaction.h"
#include "Engine/Level.h"
#include "Editor/EditorActorSubsystemTestSupport.h"
#include "NativeDObjectTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Editor;

	struct FAttachmentFixture
	{
		Tests::FActorEditorFixture Editor;
		DLevel* Level = nullptr;

		FAttachmentFixture()
		{
			Level = Editor.GetWorld()->GetCurrentLevel();
		}
	};

	auto ExpectTransformNear(const FTransform& Actual, const FTransform& Expected) -> void
	{
		EXPECT_NEAR(Actual.Translation.x, Expected.Translation.x, 1e-9);
		EXPECT_NEAR(Actual.Translation.y, Expected.Translation.y, 1e-9);
		EXPECT_NEAR(Actual.Translation.z, Expected.Translation.z, 1e-9);
		EXPECT_NEAR(Actual.Scale3D.x, Expected.Scale3D.x, 1e-9);
		EXPECT_NEAR(Actual.Scale3D.y, Expected.Scale3D.y, 1e-9);
		EXPECT_NEAR(Actual.Scale3D.z, Expected.Scale3D.z, 1e-9);
		EXPECT_NEAR(Actual.Rotation.x, Expected.Rotation.x, 1e-9);
		EXPECT_NEAR(Actual.Rotation.y, Expected.Rotation.y, 1e-9);
		EXPECT_NEAR(Actual.Rotation.z, Expected.Rotation.z, 1e-9);
		EXPECT_NEAR(Actual.Rotation.w, Expected.Rotation.w, 1e-9);
	}
}

TEST(FEditorActorAttachmentTests, MultiActorReparentAndDetachRoundTripAsTransactions)
{
	FAttachmentFixture Fixture;
	auto* OldParent = Fixture.Level->SpawnActor<ACameraActor>("OldParent");
	auto* NewParent = Fixture.Level->SpawnActor<ACameraActor>("NewParent");
	auto* First = Fixture.Level->SpawnActor<ACameraActor>("First");
	auto* Second = Fixture.Level->SpawnActor<ACameraActor>("Second");
	ASSERT_NE(OldParent, nullptr);
	ASSERT_NE(NewParent, nullptr);
	ASSERT_NE(First, nullptr);
	ASSERT_NE(Second, nullptr);
	ASSERT_TRUE(First->AttachToActor(OldParent));

	FTransform FirstTransform;
	FirstTransform.Translation = {2.0, 3.0, 4.0};
	FTransform SecondTransform;
	SecondTransform.Translation = {-5.0, 6.0, 7.0};
	ASSERT_TRUE(First->SetActorTransform(FirstTransform));
	ASSERT_TRUE(Second->SetActorTransform(SecondTransform));

	auto* Transactions = Fixture.Editor.GetTransactions();
	auto* Editor = Fixture.Editor.GetActorEditor();
	std::array<AActor*, 2> Actors{First, Second};
	ASSERT_TRUE(Editor->AttachActors(*Fixture.Level, Actors, NewParent, EAttachmentTransformRule::KeepWorld, {false}));
	EXPECT_EQ(Transactions->GetUndoContext(false).Description, "Attach actors");
	EXPECT_EQ(First->GetAttachParentActor(), NewParent);
	EXPECT_EQ(Second->GetAttachParentActor(), NewParent);
	ExpectTransformNear(First->GetActorTransform(), FirstTransform);
	ExpectTransformNear(Second->GetActorTransform(), SecondTransform);

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(First->GetAttachParentActor(), OldParent);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);
	ExpectTransformNear(First->GetActorTransform(), FirstTransform);
	ExpectTransformNear(Second->GetActorTransform(), SecondTransform);

	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(First->GetAttachParentActor(), NewParent);
	EXPECT_EQ(Second->GetAttachParentActor(), NewParent);

	ASSERT_TRUE(Editor->DetachActors(*Fixture.Level, Actors, EDetachmentTransformRule::KeepWorld, {false}));
	EXPECT_EQ(Transactions->GetUndoContext(false).Description, "Detach actors");
	EXPECT_EQ(First->GetAttachParentActor(), nullptr);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(First->GetAttachParentActor(), NewParent);
	EXPECT_EQ(Second->GetAttachParentActor(), NewParent);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(First->GetAttachParentActor(), nullptr);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);
}

TEST(FEditorActorAttachmentTests, SelectedTransformRulesProduceReversibleResults)
{
	FAttachmentFixture Fixture;
	auto* Parent = Fixture.Level->SpawnActor<ACameraActor>("Parent");
	auto* Child = Fixture.Level->SpawnActor<ACameraActor>("Child");
	ASSERT_NE(Parent, nullptr);
	ASSERT_NE(Child, nullptr);
	FTransform ParentTransform;
	ParentTransform.Translation = {10.0, 20.0, 30.0};
	FTransform ChildTransform;
	ChildTransform.Translation = {1.0, 2.0, 3.0};
	ASSERT_TRUE(Parent->SetActorTransform(ParentTransform));
	ASSERT_TRUE(Child->SetActorTransform(ChildTransform));

	auto* Transactions = Fixture.Editor.GetTransactions();
	auto* Editor = Fixture.Editor.GetActorEditor();
	AActor* Target = Child;
	ASSERT_TRUE(Editor->AttachActors(*Fixture.Level, {&Target, 1}, Parent, EAttachmentTransformRule::KeepRelative, {false}));
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	FTransform CombinedTransform = FTransform::Combine(ParentTransform, ChildTransform);
	ExpectTransformNear(Child->GetActorTransform(), CombinedTransform);

	ASSERT_TRUE(Child->DetachFromActor(EDetachmentTransformRule::KeepWorld));
	const auto Rejected = Transactions->UndoExpected(0);
	ASSERT_FALSE(Rejected);
	ASSERT_TRUE(Rejected.ApplyCause);
	const auto& Message = Rejected.ApplyCause->Message;
	EXPECT_EQ(Message, "The actor parent changed outside history.");
	EXPECT_EQ(Child->GetAttachParentActor(), nullptr);
	ExpectTransformNear(Child->GetActorTransform(), CombinedTransform);
	EXPECT_TRUE(Transactions->CanUndo());
	ASSERT_TRUE(Child->AttachToActor(Parent, EAttachmentTransformRule::KeepWorld));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Message, "The actor parent changed outside history.");
	EXPECT_EQ(Child->GetAttachParentActor(), nullptr);
	ExpectTransformNear(Child->GetActorTransform(), ChildTransform);

	ASSERT_TRUE(Editor->AttachActors(*Fixture.Level, {&Target, 1}, Parent, EAttachmentTransformRule::SnapToTarget, {false}));
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	ExpectTransformNear(Child->GetActorTransform(), ParentTransform);
	ASSERT_TRUE(Transactions->Undo());

	ASSERT_TRUE(Child->AttachToActor(Parent, EAttachmentTransformRule::KeepRelative));
	CombinedTransform = Child->GetActorTransform();
	ASSERT_TRUE(Editor->DetachActors(*Fixture.Level, {&Target, 1}, EDetachmentTransformRule::KeepRelative, {false}));
	EXPECT_EQ(Child->GetAttachParentActor(), nullptr);
	ExpectTransformNear(Child->GetActorTransform(), ChildTransform);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	ExpectTransformNear(Child->GetActorTransform(), CombinedTransform);
}

TEST(FEditorActorAttachmentTests, SnapToExistingParentStillChangesTransform)
{
	FAttachmentFixture Fixture;
	auto* Parent = Fixture.Level->SpawnActor<ACameraActor>("SnapParent");
	auto* Child = Fixture.Level->SpawnActor<ACameraActor>("SnapChild");
	ASSERT_TRUE(Child->AttachToActor(Parent));
	FTransform Before;
	Before.Translation = {4.0, 5.0, 6.0};
	ASSERT_TRUE(Child->SetActorTransform(Before));
	auto* Transactions = Fixture.Editor.GetTransactions();
	auto* Editor = Fixture.Editor.GetActorEditor();
	AActor* Target = Child;
	const auto Result = Editor->AttachActors(*Fixture.Level, {&Target, 1}, Parent, EAttachmentTransformRule::SnapToTarget, {false});
	ASSERT_TRUE(Result);
	EXPECT_TRUE(Fixture.Editor.GetTransactions()->CanUndo());
	ExpectTransformNear(Child->GetActorTransform(), Parent->GetActorTransform());
	ASSERT_TRUE(Transactions->Undo());
	ExpectTransformNear(Child->GetActorTransform(), Before);
}
