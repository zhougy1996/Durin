#include "Editor/ReflectedPropertyEditing.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectArray.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	struct FValueContainer
	{
		Durin::int32 Value = 0;
	};

	struct FCapturedChange
	{
		Durin::EPropertyChangePhase Phase = Durin::EPropertyChangePhase::Committed;
		Durin::EPropertyChangeKind Kind = Durin::EPropertyChangeKind::ValueSet;
		std::vector<Durin::uint8> MapKeyData;
	};

	class DEditObserver final : public Durin::DObject
	{
	public:
		auto PostEditChangeProperty(const Durin::FPropertyChangedEvent& Event) -> void override
		{
			FCapturedChange& Change = Changes.emplace_back();
			Change.Phase = Event.Phase;
			Change.Kind = Event.Kind;
			if (!Event.Path.empty()) Change.MapKeyData.assign(Event.Path.front().MapKeyData.begin(), Event.Path.front().MapKeyData.end());
		}

		std::vector<FCapturedChange> Changes;
	};

	class FRejectingMutationAdapter final : public Durin::IReflectedPropertyMutationAdapter
	{
	public:
		auto Capture(const Durin::FReflectedPropertyEditTarget& Target, Durin::FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool override
		{
			return Durin::CapturePropertyValue(Target.LeafProperty, Target.LeafContainer, Target.LeafArrayIndex, OutSnapshot, OutError);
		}
		auto Apply(const Durin::FReflectedPropertyEditTarget&, const Durin::FPropertyValueSnapshot&, std::string* OutError) const -> bool override
		{
			if (OutError) *OutError = "Rejected for testing.";
			return false;
		}
		auto Restore(const Durin::FReflectedPropertyEditTarget&, const Durin::FPropertyValueSnapshot&, std::string*) const -> bool override
		{
			return false;
		}
	};

	auto MakeValueProperty() -> std::unique_ptr<Durin::FNumericProperty>
	{
		return std::make_unique<Durin::FNumericProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<Durin::uint16>(offsetof(FValueContainer, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
	}

	auto CaptureValue(const Durin::FProperty* Property, FValueContainer& Container, Durin::int32 Value) -> Durin::FPropertyValueSnapshot
	{
		FValueContainer Proposed{Value};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Property, &Proposed, 0, Snapshot));
		return Snapshot;
	}

	auto MakeTarget(DEditObserver& Object, const Durin::FProperty* Property, FValueContainer& Container) -> Durin::FReflectedPropertyEditTarget
	{
		Durin::FReflectedPropertyEditTarget Target;
		Target.Object = &Object;
		Target.MemberProperty = Property;
		Target.LeafProperty = Property;
		Target.LeafContainer = &Container;
		Target.Path.push_back({Property});
		return Target;
	}
}

TEST(FReflectedPropertyEditSessionTests, AppliesInteractiveValuesAndCommitsOnce)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value"));
	const Durin::FPropertyValueSnapshot Proposed = CaptureValue(Property.get(), Container, 19);

	EXPECT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 19);
	EXPECT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::NoChange);
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_FALSE(Session.IsActive());
}

TEST(FReflectedPropertyEditSessionTests, CancelRestoresOriginalValueAndOwnedPathData)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget Target = MakeTarget(Object, Property.get(), Container);
	Target.Path[0].Selector = Durin::EPropertyPathSelector::MapKey;
	Target.Path[0].MapKeyData = {1, 2, 3};
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Nested Value"));
	Target.Path[0].MapKeyData[0] = 9;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 11)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 3);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].MapKeyData, (std::vector<Durin::uint8>{1, 2, 3}));
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, RejectsMutationWithoutChangingOrNotifying)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	DEditObserver Object;
	FRejectingMutationAdapter Adapter;
	Durin::FReflectedPropertyEditSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", &Adapter, &Error)) << Error;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8), &Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(Container.Value, 5);
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
}

TEST(FReflectedPropertyEditSessionTests, NoOpCommitAndSessionDestructionDoNotAbandonPreviewState)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{13};
	DEditObserver Object;
	{
		Durin::FReflectedPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "No-op Edit"));
		EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
		EXPECT_TRUE(Object.Changes.empty());
	}

	{
		Durin::FReflectedPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Abandoned Preview"));
		EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 27)), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Container.Value, 27);
	}
	EXPECT_EQ(Container.Value, 13);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, KeepsRegisteredTargetAliveForTheSession)
{
	InitializeDObjectSystem();
	auto Property = MakeValueProperty();
	FValueContainer Container{4};
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PropertyEditTarget"));
	Durin::FReflectedPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.LeafContainer = &Container;
	Target.Path.push_back({Property.get()});

	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Rooted Edit"));
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Object));
	ASSERT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::NoChange);
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Object));
}
