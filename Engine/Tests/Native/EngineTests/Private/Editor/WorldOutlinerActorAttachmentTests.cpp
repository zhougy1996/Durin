#include <gtest/gtest.h>

#include "Actors/CameraActor.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/Transaction.h"
#include "Engine/Level.h"
#include "Panels/ActorAttachmentTransaction.h"
#include "NativeDObjectTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Editor;
	using namespace Durin::Editor::Level;

	struct FAttachmentFixture
	{
		DLevel* Level = nullptr;

		FAttachmentFixture()
		{
			Testing::InitializeDObjectSystemForTests();
			Level = NewObject<DLevel>(nullptr, "OutlinerAttachmentLevel");
		}

		~FAttachmentFixture()
		{
			MarkObjectHierarchyAsGarbage(Level);
			CollectGarbage();
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

TEST(WorldOutlinerActorAttachmentTests, MultiActorReparentAndDetachRoundTripAsTransactions)
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

	FTransactionManager Transactions;
	std::vector<FActorAttachmentTransaction::FEntry> AttachEntries{
		{First, OldParent, NewParent, FirstTransform, FirstTransform},
		{Second, nullptr, NewParent, SecondTransform, SecondTransform},
	};
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FActorAttachmentTransaction>(std::move(AttachEntries), true)));
	EXPECT_EQ(Transactions.GetUndoDescription(), "Attach actors");
	EXPECT_EQ(First->GetAttachParentActor(), NewParent);
	EXPECT_EQ(Second->GetAttachParentActor(), NewParent);
	ExpectTransformNear(First->GetActorTransform(), FirstTransform);
	ExpectTransformNear(Second->GetActorTransform(), SecondTransform);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(First->GetAttachParentActor(), OldParent);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);
	ExpectTransformNear(First->GetActorTransform(), FirstTransform);
	ExpectTransformNear(Second->GetActorTransform(), SecondTransform);

	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(First->GetAttachParentActor(), NewParent);
	EXPECT_EQ(Second->GetAttachParentActor(), NewParent);

	std::vector<FActorAttachmentTransaction::FEntry> DetachEntries{
		{First, NewParent, nullptr, FirstTransform, FirstTransform},
		{Second, NewParent, nullptr, SecondTransform, SecondTransform},
	};
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FActorAttachmentTransaction>(std::move(DetachEntries), false)));
	EXPECT_EQ(Transactions.GetUndoDescription(), "Detach actors");
	EXPECT_EQ(First->GetAttachParentActor(), nullptr);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(First->GetAttachParentActor(), NewParent);
	EXPECT_EQ(Second->GetAttachParentActor(), NewParent);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(First->GetAttachParentActor(), nullptr);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);
}

TEST(WorldOutlinerActorAttachmentTests, SelectedTransformRulesProduceReversibleResults)
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

	FTransactionManager Transactions;
	std::vector<FActorAttachmentTransaction::FEntry> Entries;
	Entries.push_back(MakeActorAttachmentEntry(
		*Child, *Parent, EAttachmentTransformRule::KeepRelative));
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FActorAttachmentTransaction>(std::move(Entries), true)));
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	FTransform CombinedTransform = FTransform::Combine(ParentTransform, ChildTransform);
	ExpectTransformNear(Child->GetActorTransform(), CombinedTransform);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Child->GetAttachParentActor(), nullptr);
	ExpectTransformNear(Child->GetActorTransform(), ChildTransform);

	Entries.push_back(MakeActorAttachmentEntry(
		*Child, *Parent, EAttachmentTransformRule::SnapToTarget));
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FActorAttachmentTransaction>(std::move(Entries), true)));
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	ExpectTransformNear(Child->GetActorTransform(), ParentTransform);
	ASSERT_TRUE(Transactions.Undo());

	ASSERT_TRUE(Child->AttachToActor(Parent, EAttachmentTransformRule::KeepRelative));
	CombinedTransform = Child->GetActorTransform();
	Entries.push_back(MakeActorDetachmentEntry(
		*Child, EDetachmentTransformRule::KeepRelative));
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FActorAttachmentTransaction>(std::move(Entries), false)));
	EXPECT_EQ(Child->GetAttachParentActor(), nullptr);
	ExpectTransformNear(Child->GetActorTransform(), ChildTransform);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Child->GetAttachParentActor(), Parent);
	ExpectTransformNear(Child->GetActorTransform(), CombinedTransform);
}
