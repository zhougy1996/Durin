#include "ReflectedPropertyEditingTestSupport.h"
#include "PropertyEditor/PropertyHandle.h"
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
	FTestTransactorOwner Transactions;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(Durin::FPropertyEditTarget::ForMember(Component, Priority), "Edit Volumetric Cloud Priority", *Transactions.Get()));
	EXPECT_TRUE(Session.Apply(Proposed));
	EXPECT_TRUE(Session.Commit());
	EXPECT_EQ(Component->GetPriority(), 75);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Component->GetPriority(), 0);
	ASSERT_TRUE(Transactions.Get()->Redo());
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
	Durin::PropertyEditor::FPropertyEdit FrequencySession;
	ASSERT_TRUE(FrequencySession.Begin(Durin::FPropertyEditTarget::ForMember(Component, BaseFrequency)
			.ForStructMember(BaseFrequencyX), "Edit Volumetric Cloud Base Frequency", *Transactions.Get()));
	EXPECT_TRUE(FrequencySession.Apply(ProposedFrequency));
	EXPECT_TRUE(FrequencySession.Commit());
	EXPECT_EQ(Component->GetBaseFrequency(),
		Durin::FVector3f(1.0f, 0.25f, 0.5f));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Component->GetBaseFrequency(), Durin::FVector3f(0.00008f));
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Component->GetBaseFrequency(),
		Durin::FVector3f(1.0f, 0.25f, 0.5f));
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Proposal);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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
	const Durin::FPropertyEditTarget Target = Durin::FPropertyEditTarget::ForMember(Camera, Projection)
		.ForStructMember(NearClip);
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(Target, "Generic Nested Edit", false));
	EXPECT_TRUE(Session.Apply(Proposed));
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 4.0f);
	EXPECT_TRUE(Session.Cancel());
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 0.1f);

	Durin::MarkAsGarbage(Camera);
	Durin::MarkAsGarbage(ProposedCamera);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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

	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(Durin::FPropertyEditTarget::ForMember(Child, Property), "Edit Transform", false));
	ASSERT_TRUE(Session.Apply(Proposed));
	EXPECT_NEAR(Durin::Math::Length(Child->GetRelativeRotation()), 1.0, 1.e-8);
	EXPECT_DOUBLE_EQ(Child->GetWorldLocation().x, 13.0);
	ASSERT_TRUE(Session.Cancel());
	EXPECT_DOUBLE_EQ(Child->GetWorldLocation().x, 10.0);

	Child->DetachFromComponent(Durin::EDetachmentTransformRule::KeepWorld);
	Durin::MarkAsGarbage(ProposalObject);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FReflectedPropertyEditSessionTests, FailedPublicationLeavesPartialWriteUntilGestureCancel)
{
	using namespace Durin;
	using FValues = std::vector<int32>;
	FArrayOps Ops = *ResolveArrayOps<FValues>();
	static void* FailingDestination = nullptr;
	Ops.Commit = [](void* Destination, void* Detached) {
		if (Destination == FailingDestination)
		{
			FailingDestination = nullptr;
			static_cast<FValues*>(Destination)->front() = 17;
			return EContainerOpResult::BackendRejected;
		}
		return ResolveArrayOps<FValues>()->Commit(Destination, Detached);
	};
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner, &Ops);
	FArrayValueContainer Container{{7}};
	FManagedEditObserver Object;
	auto Target = FPropertyEditTarget::ForMember(Object.Get(), Array.get());
	Target.SnapshotContainer = &Container;
	PropertyEditor::FPropertyEdit Edit;
	ASSERT_TRUE(Edit.Begin(Target.ForArrayElement(Inner.get(), 0), "Array edit", false));

	std::string Error;
	FailingDestination = &Container.Values;
	const bool bApplied = Edit.SetValue(int32{19}, &Error);
	FailingDestination = nullptr;
	EXPECT_FALSE(bApplied);
	EXPECT_NE(Error.find("Commit"), std::string::npos) << Error;
	EXPECT_EQ(Container.Values, FValues{17});
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_FALSE(Edit.GetCurrentValue().IsValid());
	ASSERT_TRUE(Edit.Cancel(&Error)) << Error;
	EXPECT_EQ(Container.Values, FValues{7});
}

TEST(FReflectedPropertyEditSessionTests, ArrayElementUsesStableContainerSnapshotAndExactPathDuringEdit)
{
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner);
	FArrayValueContainer Container{{3, 7, 11}};
	FManagedEditObserver Object;
	Durin::FPropertyEditTarget ArrayTarget = Durin::FPropertyEditTarget::ForMember(Object.Get(), Array.get());
	ArrayTarget.SnapshotContainer = &Container;
	Durin::FPropertyEditTarget ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(ElementTarget, "Edit Array Element", false));

	FArrayValueContainer FirstProposal{{3, 19, 11}};
	Durin::FPropertyValueSnapshot FirstSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &FirstProposal, 0, FirstSnapshot));
	ASSERT_TRUE(Session.Apply(FirstSnapshot));
	// Whole-container restore is allowed to move storage, but path identity must
	// keep the same continuous widget in one edit session.
	ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	EXPECT_TRUE(Session.MatchesTarget(ElementTarget));
	FArrayValueContainer SecondProposal{{3, 23, 11}};
	Durin::FPropertyValueSnapshot SecondSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &SecondProposal, 0, SecondSnapshot));
	ASSERT_TRUE(Session.Apply(SecondSnapshot));
	ASSERT_TRUE(Session.Commit());
	ASSERT_EQ(Container.Values, (std::vector<int32>{3, 23, 11}));

	ASSERT_EQ(Object.Changes.size(), 3u);
	const FCapturedChange& Commit = Object.Changes.back();
	EXPECT_EQ(Commit.MemberProperty, Array.get());
	EXPECT_EQ(Commit.LeafProperty, Inner.get());
	ASSERT_EQ(Commit.Selectors.size(), 2u);
	EXPECT_EQ(Commit.Selectors[0], Durin::EPropertyPathSelector::ArrayIndex);
	EXPECT_EQ(Commit.Indices[0], 1u);
	EXPECT_EQ(Commit.Selectors[1], Durin::EPropertyPathSelector::None);
}

TEST(FReflectedPropertyEditSessionTests, ArrayStructuralKindsPublishCommittedElements)
{
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner);
	FArrayValueContainer Container{{4, 8, 15}};
	FManagedEditObserver Object;
	Durin::FPropertyEditTarget Target = Durin::FPropertyEditTarget::ForMember(Object.Get(), Array.get());
	Target.SnapshotContainer = &Container;
	auto CommitValues = [&](std::vector<int32> Values, Durin::EPropertyChangeType::Type ChangeType) {
		Target.ChangeType = ChangeType;
		FArrayValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Array.get(), &Proposed, 0, Snapshot));
		Durin::PropertyEditor::FPropertyEdit Session;
		ASSERT_TRUE(Session.Begin(Target, "Edit Array Structure", false));
		ASSERT_TRUE(Session.Apply(Snapshot));
		ASSERT_FALSE(Object.Changes.empty());
		EXPECT_EQ(Object.Changes.back().ChangeType, ChangeType | Durin::EPropertyChangeType::Interactive);
		EXPECT_TRUE(Session.Commit());
		EXPECT_EQ(Object.Changes.back().ChangeType, ChangeType);
	};

	CommitValues({4, 8, 15, 16}, Durin::EPropertyChangeType::ArrayAdd);
	EXPECT_EQ(Container.Values.size(), 4u);
	CommitValues({4, 8}, Durin::EPropertyChangeType::ArrayRemove);
	EXPECT_EQ(Container.Values, (std::vector<int32>{4, 8}));
}

TEST(FReflectedPropertyEditSessionTests, MapEditsPreserveStableKeyPathsAndStructuralKinds)
{
	Durin::FStringProperty KeyProperty(
		Durin::FFieldVariant(), Durin::FName("Key"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
		1, 0, static_cast<uint16>(sizeof(std::string)), Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
	);
	auto ValueProperty = MakeValueProperty();
	auto MapProperty = MakeMapProperty(KeyProperty, *ValueProperty);
	FMapValueContainer Container{{{"Alpha", 1}, {"Beta", 2}}};
	FManagedEditObserver Object;
	Durin::FPropertyEditTarget MapTarget = Durin::FPropertyEditTarget::ForMember(Object.Get(), MapProperty.get());
	MapTarget.SnapshotContainer = &Container;
	Durin::FPropertyValueSnapshot KeySnapshot;
	const std::string Alpha = "Alpha";
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Alpha, 0, KeySnapshot));
	Durin::FPropertyEditTarget ValueTarget = MapTarget.ForMapEntry(
		ValueProperty.get(), KeySnapshot, KeySnapshot.GetBytes()
	);
	Durin::PropertyEditor::FPropertyEdit ValueSession;
	ASSERT_TRUE(ValueSession.Begin(ValueTarget, "Edit Map Value", false));
	FMapValueContainer ValueProposal{{{"Alpha", 9}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot ValueSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &ValueProposal, 0, ValueSnapshot));
	ASSERT_TRUE(ValueSession.Apply(ValueSnapshot));
	ASSERT_TRUE(ValueSession.Commit());
	ASSERT_EQ(Object.Changes.back().Selectors.size(), 2u);
	EXPECT_EQ(Object.Changes.back().Selectors[0], Durin::EPropertyPathSelector::MapKey);
	EXPECT_EQ(Object.Changes.back().MapKeyData, KeySnapshot.GetBytes());
	EXPECT_EQ(Container.Values.at("Alpha"), 9);

	auto CommitMap = [&](FStringIntMap Values, Durin::EPropertyChangeType::Type ChangeType, const std::string& PathKey) {
		Durin::FPropertyValueSnapshot StableKey;
		ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &PathKey, 0, StableKey));
		Durin::FPropertyEditTarget Target = MapTarget;
		Target.ChangeType = ChangeType;
		Target.Path.back().Selector = Durin::EPropertyPathSelector::MapKey;
		Target.Path.back().MapKeyData = StableKey.GetBytes();
		FMapValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &Proposed, 0, Snapshot));
		Durin::PropertyEditor::FPropertyEdit Session;
		ASSERT_TRUE(Session.Begin(Target, "Edit Map Structure", false));
		ASSERT_TRUE(Session.Apply(Snapshot));
		ASSERT_TRUE(Session.Commit());
		EXPECT_EQ(Object.Changes.back().ChangeType, ChangeType);
		EXPECT_EQ(Object.Changes.back().MapKeyData, StableKey.GetBytes());
	};

	CommitMap({{"Alpha", 1}, {"Beta", 2}, {"Gamma", 3}}, Durin::EPropertyChangeType::ArrayAdd, "Gamma");
	EXPECT_TRUE(Container.Values.contains("Gamma"));
	CommitMap({{"Beta", 2}}, Durin::EPropertyChangeType::ArrayRemove, "Alpha");
	EXPECT_FALSE(Container.Values.contains("Alpha"));

	Container.Values = {{"Alpha", 1}, {"Beta", 2}};
	std::string EditedKey = "Alpha";
	Durin::FPropertyEditTarget RenameTarget = MapTarget.ForMapEntry(
		&KeyProperty, KeySnapshot, KeySnapshot.GetBytes());

	Durin::PropertyEditor::FPropertyEdit RenameSession;
	ASSERT_TRUE(RenameSession.Begin(RenameTarget, "Rename Map Key", false));
	FMapValueContainer FirstRename{{{"Renamed", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FirstRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FirstRename, 0, FirstRenameSnapshot));
	std::string RenameError;
	const bool EditResult1 = RenameSession.Apply(FirstRenameSnapshot, &RenameError);
	ASSERT_TRUE(EditResult1) << RenameError;
	const std::string Renamed = "Renamed";
	Durin::FPropertyValueSnapshot RenamedKeySnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Renamed, 0, RenamedKeySnapshot));
	Durin::FPropertyEditTarget ContinuedRename = MapTarget.ForMapEntry(
		&KeyProperty, RenamedKeySnapshot, RenamedKeySnapshot.GetBytes());

	EXPECT_TRUE(RenameSession.MatchesTarget(ContinuedRename));
	FMapValueContainer FinalRename{{{"Final", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FinalRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FinalRename, 0, FinalRenameSnapshot));
	ASSERT_TRUE(RenameSession.Apply(FinalRenameSnapshot));
	ASSERT_TRUE(RenameSession.Commit());
	EXPECT_TRUE(Container.Values.contains("Final"));
	EXPECT_FALSE(Container.Values.contains("Renamed"));
}
