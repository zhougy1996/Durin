#include "ReflectedPropertyEditingTestSupport.h"
#include "Components/VolumetricCloudComponent.h"
#include "DObject/Class.h"
#include "Math/Operations.h"

TEST(FReflectedPropertyEditSessionTests,
	GenericVolumetricCloudPropertyCommitsAndUndoRedoes)
{
	InitializeDObjectSystem();
	auto* Component = Durin::NewObject<Durin::DVolumetricCloudComponent>(
		nullptr, "CloudDetailsTarget");
	auto* Proposal = Durin::NewObject<Durin::DVolumetricCloudComponent>(
		nullptr, "CloudDetailsProposal");
	auto* Priority = Component->GetClass()->FindPropertyByName(
		Durin::FName("Priority"));
	ASSERT_NE(Priority, nullptr);
	EXPECT_TRUE(Durin::EnumHasAnyFlags(
		Priority->GetPropertyFlags(), Durin::EPropertyFlags::Edit));
	Proposal->SetPriority(75);
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Priority, Proposal, 0, Proposed));
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Component, Priority),
		"Edit Volumetric Cloud Priority", nullptr, &Transactions));
	EXPECT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Component->GetPriority(), 75);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetPriority(), 0);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Component->GetPriority(), 75);

	auto* BaseFrequency = static_cast<Durin::FStructProperty*>(
		Component->GetClass()->FindPropertyByName("BaseFrequency"));
	ASSERT_NE(BaseFrequency, nullptr);
	auto* BaseFrequencyX = BaseFrequency->GetStruct()->FindPropertyByName(
		Durin::FName("x"));
	ASSERT_NE(BaseFrequencyX, nullptr);
	Proposal->SetDensityMapping(
		Durin::FVector3f(2.0f, 0.25f, 0.5f),
		Proposal->GetDetailFrequency(), Proposal->GetWindOffset(),
		Proposal->GetWeatherFrequency(), Proposal->GetWeatherOffset());
	Durin::FPropertyValueSnapshot ProposedFrequency;
	ASSERT_TRUE(Durin::CapturePropertyValue(
		BaseFrequency, Proposal, 0, ProposedFrequency));
	Durin::Editor::FPropertyEditSession FrequencySession;
	ASSERT_TRUE(FrequencySession.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Component, BaseFrequency)
			.ForStructMember(BaseFrequencyX),
		"Edit Volumetric Cloud Base Frequency", nullptr, &Transactions));
	EXPECT_EQ(FrequencySession.Apply(ProposedFrequency),
		Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(FrequencySession.Commit(),
		Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Component->GetBaseFrequency(),
		Durin::FVector3f(1.0f, 0.25f, 0.5f));
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetBaseFrequency(), Durin::FVector3f(0.00008f));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Component->GetBaseFrequency(),
		Durin::FVector3f(1.0f, 0.25f, 0.5f));
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Proposal);
	Durin::CollectGarbage();
}

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
	const Durin::Editor::FPropertyEditTarget Target = Durin::Editor::FPropertyEditTarget::ForMember(Camera, Projection)
		.ForStructMember(NearClip);
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Generic Nested Edit"));
	EXPECT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 4.0f);
	EXPECT_EQ(Session.Cancel(), Durin::Editor::EPropertyEditResult::Changed);
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

	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::Editor::FPropertyEditTarget::ForMember(Child, Property), "Edit Transform"));
	ASSERT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_NEAR(Durin::Math::Length(Child->GetRelativeRotation()), 1.0, 1.e-8);
	EXPECT_DOUBLE_EQ(Child->GetWorldLocation().x, 13.0);
	ASSERT_EQ(Session.Cancel(), Durin::Editor::EPropertyEditResult::Changed);
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
	Durin::Editor::FPropertyEditTarget ArrayTarget = Durin::Editor::FPropertyEditTarget::ForMember(&Object, Array.get());
	ArrayTarget.SnapshotContainer = &Container;
	Durin::Editor::FPropertyEditTarget ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(ElementTarget, "Edit Array Element", nullptr, &Transactions));

	FArrayValueContainer FirstProposal{{3, 19, 11}};
	Durin::FPropertyValueSnapshot FirstSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &FirstProposal, 0, FirstSnapshot));
	ASSERT_EQ(Session.Apply(FirstSnapshot), Durin::Editor::EPropertyEditResult::Changed);
	// Whole-container restore is allowed to move storage, but path identity must
	// keep the same continuous widget in one edit session.
	ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	EXPECT_TRUE(Session.MatchesTarget(ElementTarget));
	FArrayValueContainer SecondProposal{{3, 23, 11}};
	Durin::FPropertyValueSnapshot SecondSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &SecondProposal, 0, SecondSnapshot));
	ASSERT_EQ(Session.Apply(SecondSnapshot), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Container.Values, (std::vector<int32>{3, 23, 11}));

	ASSERT_EQ(Object.Changes.size(), 3u);
	const FCapturedChange& Commit = Object.Changes.back();
	EXPECT_EQ(Commit.MemberProperty, Array.get());
	EXPECT_EQ(Commit.LeafProperty, Inner.get());
	ASSERT_EQ(Commit.Selectors.size(), 2u);
	EXPECT_EQ(Commit.Selectors[0], Durin::EPropertyPathSelector::ArrayIndex);
	EXPECT_EQ(Commit.Indices[0], 1u);
	EXPECT_EQ(Commit.Selectors[1], Durin::EPropertyPathSelector::None);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<int32>{3, 7, 11}));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Values, (std::vector<int32>{3, 23, 11}));
}

TEST(FReflectedPropertyEditSessionTests, ArrayStructuralKindsRestoreRemovedAndResizedElements)
{
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner);
	FArrayValueContainer Container{{4, 8, 15}};
	DEditObserver Object;
	Durin::Editor::FPropertyEditTarget Target = Durin::Editor::FPropertyEditTarget::ForMember(&Object, Array.get());
	Target.SnapshotContainer = &Container;
	Durin::Editor::FTransactionManager Transactions;

	auto CommitValues = [&](std::vector<int32> Values, Durin::EPropertyChangeKind Kind) {
		Target.Kind = Kind;
		FArrayValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Array.get(), &Proposed, 0, Snapshot));
		Durin::Editor::FPropertyEditSession Session;
		EXPECT_TRUE(Session.Begin(Target, "Edit Array Structure", nullptr, &Transactions));
		EXPECT_EQ(Session.Apply(Snapshot), Durin::Editor::EPropertyEditResult::Changed);
		EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
		EXPECT_EQ(Object.Changes.back().Kind, Kind);
	};

	CommitValues({4, 8, 15, 16}, Durin::EPropertyChangeKind::ArrayAdd);
	EXPECT_EQ(Container.Values.size(), 4u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<int32>{4, 8, 15}));
	Transactions.Clear();
	CommitValues({4, 8}, Durin::EPropertyChangeKind::ArrayRemove);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<int32>{4, 8, 15}));
	Transactions.Clear();
	CommitValues({4}, Durin::EPropertyChangeKind::ArrayResize);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<int32>{4, 8, 15}));
}

TEST(FReflectedPropertyEditSessionTests, MapTransactionsPreserveStableKeyPathsAndStructuralKinds)
{
	Durin::FStringProperty KeyProperty(
		Durin::FFieldVariant(), Durin::FName("Key"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
		1, 0, static_cast<uint16>(sizeof(std::string)), Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
	);
	auto ValueProperty = MakeValueProperty();
	auto MapProperty = MakeMapProperty(KeyProperty, *ValueProperty);
	FMapValueContainer Container{{{"Alpha", 1}, {"Beta", 2}}};
	DEditObserver Object;
	Durin::Editor::FPropertyEditTarget MapTarget = Durin::Editor::FPropertyEditTarget::ForMember(&Object, MapProperty.get());
	MapTarget.SnapshotContainer = &Container;
	Durin::FPropertyValueSnapshot KeySnapshot;
	const std::string Alpha = "Alpha";
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Alpha, 0, KeySnapshot));
	Durin::Editor::FPropertyEditTarget ValueTarget = MapTarget.ForMapEntry(
		ValueProperty.get(), KeySnapshot, KeySnapshot.GetBytes()
	);
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession ValueSession;
	ASSERT_TRUE(ValueSession.Begin(ValueTarget, "Edit Map Value", nullptr, &Transactions));
	FMapValueContainer ValueProposal{{{"Alpha", 9}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot ValueSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &ValueProposal, 0, ValueSnapshot));
	ASSERT_EQ(ValueSession.Apply(ValueSnapshot), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(ValueSession.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Object.Changes.back().Selectors.size(), 2u);
	EXPECT_EQ(Object.Changes.back().Selectors[0], Durin::EPropertyPathSelector::MapKey);
	EXPECT_EQ(Object.Changes.back().MapKeyData, KeySnapshot.GetBytes());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values.at("Alpha"), 1);
	Transactions.Clear();

	auto CommitMap = [&](FStringIntMap Values, Durin::EPropertyChangeKind Kind, const std::string& PathKey) {
		Durin::FPropertyValueSnapshot StableKey;
		ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &PathKey, 0, StableKey));
		Durin::Editor::FPropertyEditTarget Target = MapTarget;
		Target.Kind = Kind;
		Target.Path.back().Selector = Durin::EPropertyPathSelector::MapKey;
		Target.Path.back().MapKeyData = StableKey.GetBytes();
		FMapValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &Proposed, 0, Snapshot));
		Durin::Editor::FPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(Target, "Edit Map Structure", nullptr, &Transactions));
		ASSERT_EQ(Session.Apply(Snapshot), Durin::Editor::EPropertyEditResult::Changed);
		ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
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
	Durin::Editor::FPropertyEditTarget RenameTarget = MapTarget.ForMapEntry(
		&KeyProperty, KeySnapshot, KeySnapshot.GetBytes());
	RenameTarget.Kind = Durin::EPropertyChangeKind::MapKeyRename;
	Durin::Editor::FPropertyEditSession RenameSession;
	ASSERT_TRUE(RenameSession.Begin(RenameTarget, "Rename Map Key", nullptr, &Transactions));
	FMapValueContainer FirstRename{{{"Renamed", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FirstRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FirstRename, 0, FirstRenameSnapshot));
	std::string RenameError;
	ASSERT_EQ(RenameSession.Apply(FirstRenameSnapshot, &RenameError), Durin::Editor::EPropertyEditResult::Changed) << RenameError;
	const std::string Renamed = "Renamed";
	Durin::FPropertyValueSnapshot RenamedKeySnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Renamed, 0, RenamedKeySnapshot));
	Durin::Editor::FPropertyEditTarget ContinuedRename = MapTarget.ForMapEntry(
		&KeyProperty, RenamedKeySnapshot, RenamedKeySnapshot.GetBytes());
	ContinuedRename.Kind = Durin::EPropertyChangeKind::MapKeyRename;
	EXPECT_TRUE(RenameSession.MatchesTarget(ContinuedRename));
	FMapValueContainer FinalRename{{{"Final", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FinalRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FinalRename, 0, FinalRenameSnapshot));
	ASSERT_EQ(RenameSession.Apply(FinalRenameSnapshot), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(RenameSession.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Transactions.ConsumeEvents().size(), 1u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Container.Values.contains("Alpha"));
	EXPECT_FALSE(Container.Values.contains("Final"));
}
