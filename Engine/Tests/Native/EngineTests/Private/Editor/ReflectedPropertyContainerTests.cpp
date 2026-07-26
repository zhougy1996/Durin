#include "ReflectedPropertyEditingTestSupport.h"

TEST(FReflectedPropertyEditSessionTests, GenericHookPipelineAppliesNestedStructField)
{
	InitializeDObjectSystem();
	auto* Camera = Durin::NewObject<Durin::DCameraComponent>(nullptr, "GenericNestedCamera");
	auto* ProposedCamera = Durin::NewObject<Durin::DCameraComponent>(nullptr, "GenericNestedProposal");
	auto* Projection = static_cast<Durin::FStructProperty*>(Camera->GetClass()->FindPropertyByName("ProjectionSettings"));
	ASSERT_NE(Projection, nullptr);
	auto* NearClip = Projection->GetStruct()->FindPropertyByName(Durin::FName("NearClip"));
	ASSERT_NE(NearClip, nullptr);

	Durin::FCameraProjectionSettings ProposedSettings = ProposedCamera->GetProjectionSettings();
	ProposedSettings.NearClip = 4.0f;
	ProposedCamera->SetProjectionSettings(ProposedSettings);
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Projection, ProposedCamera, 0, Proposed));
	const Durin::FReflectedPropertyEditTarget Target = Durin::FReflectedPropertyEditTarget::ForMember(Camera, Projection)
		.ForStructMember(NearClip);
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Generic Nested Edit"));
	EXPECT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 4.0f);
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 0.1f);

	Durin::MarkAsGarbage(Camera);
	Durin::MarkAsGarbage(ProposedCamera);
	Durin::CollectGarbage();
}

TEST(FReflectedPropertyEditSessionTests, RelativeTransformHookNormalizesAndRefreshesHierarchyAcrossCancel)
{
	InitializeDObjectSystem();
	auto* Parent = Durin::NewObject<Durin::DSceneComponent>(nullptr, "HookTransformParent");
	auto* Child = Durin::NewObject<Durin::DSceneComponent>(nullptr, "HookTransformChild");
	auto* ProposalObject = Durin::NewObject<Durin::DSceneComponent>(nullptr, "HookTransformProposal");
	Durin::FTransform ParentTransform;
	ParentTransform.Translation = {10.0, 0.0, 0.0};
	Parent->SetRelativeTransform(ParentTransform);
	ASSERT_TRUE(Child->SetupAttachment(Parent));

	auto* Property = Child->GetClass()->FindPropertyByName(Durin::FName("RelativeTransform"));
	ASSERT_NE(Property, nullptr);
	auto* ProposedTransform = Property->ContainerPtrToValuePtr<Durin::FTransform>(ProposalObject);
	ProposedTransform->Translation = {3.0, 0.0, 0.0};
	ProposedTransform->Rotation *= 2.0;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, ProposalObject, 0, Proposed));

	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::FReflectedPropertyEditTarget::ForMember(Child, Property), "Edit Transform"));
	ASSERT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_NEAR(glm::length(Child->GetRelativeRotation()), 1.0, 1.e-8);
	EXPECT_DOUBLE_EQ(Child->GetWorldLocation().x, 13.0);
	ASSERT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_DOUBLE_EQ(Child->GetWorldLocation().x, 10.0);

	Child->DetachFromComponent(Durin::EDetachmentTransformRule::KeepWorld);
	Durin::MarkAsGarbage(ProposalObject);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::CollectGarbage();
}

TEST(FReflectedPropertyEditSessionTests, ArrayElementUsesStableContainerSnapshotAndExactPath)
{
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner);
	FArrayValueContainer Container{{3, 7, 11}};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget ArrayTarget = Durin::FReflectedPropertyEditTarget::ForMember(&Object, Array.get());
	ArrayTarget.SnapshotContainer = &Container;
	Durin::FReflectedPropertyEditTarget ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(ElementTarget, "Edit Array Element", nullptr, &Transactions));

	FArrayValueContainer FirstProposal{{3, 19, 11}};
	Durin::FPropertyValueSnapshot FirstSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &FirstProposal, 0, FirstSnapshot));
	ASSERT_EQ(Session.Apply(FirstSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	// Whole-container restore is allowed to move storage, but path identity must
	// keep the same continuous widget in one edit session.
	ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	EXPECT_TRUE(Session.MatchesTarget(ElementTarget));
	FArrayValueContainer SecondProposal{{3, 23, 11}};
	Durin::FPropertyValueSnapshot SecondSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &SecondProposal, 0, SecondSnapshot));
	ASSERT_EQ(Session.Apply(SecondSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Container.Values, (std::vector<Durin::int32>{3, 23, 11}));

	ASSERT_EQ(Object.Changes.size(), 3u);
	const FCapturedChange& Commit = Object.Changes.back();
	EXPECT_EQ(Commit.MemberProperty, Array.get());
	EXPECT_EQ(Commit.LeafProperty, Inner.get());
	ASSERT_EQ(Commit.Selectors.size(), 2u);
	EXPECT_EQ(Commit.Selectors[0], Durin::EPropertyPathSelector::ArrayIndex);
	EXPECT_EQ(Commit.Indices[0], 1u);
	EXPECT_EQ(Commit.Selectors[1], Durin::EPropertyPathSelector::None);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{3, 7, 11}));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{3, 23, 11}));
}

TEST(FReflectedPropertyEditSessionTests, ArrayStructuralKindsRestoreRemovedAndResizedElements)
{
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner);
	FArrayValueContainer Container{{4, 8, 15}};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget Target = Durin::FReflectedPropertyEditTarget::ForMember(&Object, Array.get());
	Target.SnapshotContainer = &Container;
	Durin::FEditorTransactionManager Transactions;

	auto CommitValues = [&](std::vector<Durin::int32> Values, Durin::EPropertyChangeKind Kind) {
		Target.Kind = Kind;
		FArrayValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Array.get(), &Proposed, 0, Snapshot));
		Durin::FReflectedPropertyEditSession Session;
		EXPECT_TRUE(Session.Begin(Target, "Edit Array Structure", nullptr, &Transactions));
		EXPECT_EQ(Session.Apply(Snapshot), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Object.Changes.back().Kind, Kind);
	};

	CommitValues({4, 8, 15, 16}, Durin::EPropertyChangeKind::ArrayAdd);
	EXPECT_EQ(Container.Values.size(), 4u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{4, 8, 15}));
	Transactions.Clear();
	CommitValues({4, 8}, Durin::EPropertyChangeKind::ArrayRemove);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{4, 8, 15}));
	Transactions.Clear();
	CommitValues({4}, Durin::EPropertyChangeKind::ArrayResize);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{4, 8, 15}));
}

TEST(FReflectedPropertyEditSessionTests, MapTransactionsPreserveStableKeyPathsAndStructuralKinds)
{
	Durin::FStringProperty KeyProperty(
		Durin::FFieldVariant(), Durin::FName("Key"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
		1, 0, static_cast<Durin::uint16>(sizeof(std::string)), Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
	);
	auto ValueProperty = MakeValueProperty();
	auto MapProperty = MakeMapProperty(KeyProperty, *ValueProperty);
	FMapValueContainer Container{{{"Alpha", 1}, {"Beta", 2}}};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget MapTarget = Durin::FReflectedPropertyEditTarget::ForMember(&Object, MapProperty.get());
	MapTarget.SnapshotContainer = &Container;
	Durin::FPropertyValueSnapshot KeySnapshot;
	const std::string Alpha = "Alpha";
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Alpha, 0, KeySnapshot));
	Durin::FReflectedPropertyEditTarget ValueTarget = MapTarget.ForMapEntry(
		ValueProperty.get(), KeySnapshot, KeySnapshot.GetBytes()
	);
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession ValueSession;
	ASSERT_TRUE(ValueSession.Begin(ValueTarget, "Edit Map Value", nullptr, &Transactions));
	FMapValueContainer ValueProposal{{{"Alpha", 9}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot ValueSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &ValueProposal, 0, ValueSnapshot));
	ASSERT_EQ(ValueSession.Apply(ValueSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(ValueSession.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Object.Changes.back().Selectors.size(), 2u);
	EXPECT_EQ(Object.Changes.back().Selectors[0], Durin::EPropertyPathSelector::MapKey);
	EXPECT_EQ(Object.Changes.back().MapKeyData, KeySnapshot.GetBytes());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values.at("Alpha"), 1);
	Transactions.Clear();

	auto CommitMap = [&](FStringIntMap Values, Durin::EPropertyChangeKind Kind, const std::string& PathKey) {
		Durin::FPropertyValueSnapshot StableKey;
		ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &PathKey, 0, StableKey));
		Durin::FReflectedPropertyEditTarget Target = MapTarget;
		Target.Kind = Kind;
		Target.Path.back().Selector = Durin::EPropertyPathSelector::MapKey;
		Target.Path.back().MapKeyData = StableKey.GetBytes();
		FMapValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &Proposed, 0, Snapshot));
		Durin::FReflectedPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(Target, "Edit Map Structure", nullptr, &Transactions));
		ASSERT_EQ(Session.Apply(Snapshot), Durin::EReflectedPropertyEditResult::Changed);
		ASSERT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Object.Changes.back().Kind, Kind);
		EXPECT_EQ(Object.Changes.back().MapKeyData, StableKey.GetBytes());
	};

	CommitMap({{"Alpha", 1}, {"Beta", 2}, {"Gamma", 3}}, Durin::EPropertyChangeKind::MapInsert, "Gamma");
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Container.Values.contains("Gamma"));
	Transactions.Clear();
	CommitMap({{"Beta", 2}}, Durin::EPropertyChangeKind::MapRemove, "Alpha");
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Container.Values.contains("Alpha"));
	Transactions.Clear();

	std::string EditedKey = "Alpha";
	Durin::FReflectedPropertyEditTarget RenameTarget = MapTarget.ForMapEntry(
		&KeyProperty, KeySnapshot, KeySnapshot.GetBytes());
	RenameTarget.Kind = Durin::EPropertyChangeKind::MapKeyRename;
	Durin::FReflectedPropertyEditSession RenameSession;
	ASSERT_TRUE(RenameSession.Begin(RenameTarget, "Rename Map Key", nullptr, &Transactions));
	FMapValueContainer FirstRename{{{"Renamed", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FirstRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FirstRename, 0, FirstRenameSnapshot));
	std::string RenameError;
	ASSERT_EQ(RenameSession.Apply(FirstRenameSnapshot, &RenameError), Durin::EReflectedPropertyEditResult::Changed) << RenameError;
	const std::string Renamed = "Renamed";
	Durin::FPropertyValueSnapshot RenamedKeySnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Renamed, 0, RenamedKeySnapshot));
	Durin::FReflectedPropertyEditTarget ContinuedRename = MapTarget.ForMapEntry(
		&KeyProperty, RenamedKeySnapshot, RenamedKeySnapshot.GetBytes());
	ContinuedRename.Kind = Durin::EPropertyChangeKind::MapKeyRename;
	EXPECT_TRUE(RenameSession.MatchesTarget(ContinuedRename));
	FMapValueContainer FinalRename{{{"Final", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FinalRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FinalRename, 0, FinalRenameSnapshot));
	ASSERT_EQ(RenameSession.Apply(FinalRenameSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(RenameSession.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Transactions.ConsumeEvents().size(), 1u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Container.Values.contains("Alpha"));
	EXPECT_FALSE(Container.Values.contains("Final"));
}
