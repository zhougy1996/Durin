#include <gtest/gtest.h>
#include "Editor/EditorTransactionTestSupport.h"

#include "Actors/CameraActor.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Transactions/Transaction.h"
#include "Engine/Level.h"
#include "EditorSupportDelegates.h"
#include "Editor/EditorActorSubsystemTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin;

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

TEST(FEditorActorAttachmentTests, CanceledBatchStillChangesAndDirtiesEachChildWithoutUndoHistory)
{
	Tests::FActorEditorFixture Fixture;
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/AttachmentBatch/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AttachmentBatch/Level", Path));
	TStrongObjectPtr<DPackage> Package(CreatePackage(Path));
	ASSERT_TRUE(Package);
	auto* Level = NewObject<DLevel>(Package.Get(), "Level");
	ASSERT_TRUE(Fixture.GetWorld()->SetCurrentLevel(Level));
	auto* Parent = Level->SpawnActor<ACameraActor>("Parent");
	std::array<AActor*, 2> Children{Level->SpawnActor<ACameraActor>("First"), Level->SpawnActor<ACameraActor>("Second")};
	ASSERT_TRUE(Parent && Children[0] && Children[1]);
	auto* Transactions = Fixture.GetTransactions();
	auto* Editor = Fixture.GetActorEditor();
	for (const bool bAttach : {true, false})
	{
		Package->ClearDirty();
		const auto Revision = Package->GetEditRevision();
		AActor* ExpectedParent = bAttach ? Parent : nullptr;
		int Cancellations = 0;
		const auto Handle = OnAssetPackagesChanged().AddLambda([&] {
			if (!Transactions->IsActive() || !Package->IsDirty()) return;
			EXPECT_EQ(Children[0]->GetAttachParentActor(), ExpectedParent);
			EXPECT_NE(Children[1]->GetAttachParentActor(), ExpectedParent);
			++Cancellations;
			Transactions->Cancel();
		});
		const bool bChanged = bAttach
			? Editor->AttachActors(*Level, Children, Parent, EAttachmentTransformRule::KeepWorld, {false})
			: Editor->DetachActors(*Level, Children, EDetachmentTransformRule::KeepWorld, {false});
		OnAssetPackagesChanged().Remove(Handle);
		EXPECT_TRUE(bChanged);
		EXPECT_EQ(Cancellations, 1);
		for (auto* Child : Children) EXPECT_EQ(Child->GetAttachParentActor(), ExpectedParent);
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_EQ(Package->GetEditRevision(), Revision + Children.size());
		EXPECT_EQ(Transactions->GetQueueLength(), 0u);
	}
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
	ASSERT_TRUE(Transactions->Undo());
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

TEST(FEditorActorAttachmentTests, LaterCycleSkipsActorAndOneUndoRestoresEarlierAttachment)
{
	FAttachmentFixture F;
	auto* Old = F.Level->SpawnActor<ACameraActor>("Old");
	auto* Destination = F.Level->SpawnActor<ACameraActor>("Destination");
	auto* First = F.Level->SpawnActor<ACameraActor>("First");
	auto* Second = F.Level->SpawnActor<ACameraActor>("Second");
	ASSERT_TRUE(First->AttachToActor(Old));
	bool Injected = false;
	auto Handle = FEditorSupportDelegates::ActorHierarchyChanged.AddLambda([&](DLevel& Level) {
		if (&Level != F.Level || Injected || First->GetAttachParentActor() != Destination) return;
		Injected = true;
		EXPECT_TRUE(Destination->AttachToActor(Second)); // Block the next forward write.
	});
	std::array<AActor*, 2> Actors{First, Second};
	auto Result = F.Editor.GetActorEditor()->AttachActors(*F.Level, Actors, Destination, EAttachmentTransformRule::KeepWorld, {false});
	FEditorSupportDelegates::ActorHierarchyChanged.Remove(Handle);
	EXPECT_TRUE(Injected);
	EXPECT_TRUE(Result);
	EXPECT_EQ(First->GetAttachParentActor(), Destination);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);
	ASSERT_EQ(F.Editor.GetTransactions()->GetQueueLength(), 1u);
	ASSERT_TRUE(F.Editor.GetTransactions()->Undo());
	EXPECT_EQ(First->GetAttachParentActor(), Old);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);
	EXPECT_FALSE(F.Editor.GetTransactions()->CanUndo());
}
