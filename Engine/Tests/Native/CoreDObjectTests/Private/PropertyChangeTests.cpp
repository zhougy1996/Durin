#include "DObject/Object.h"
#include "DObject/Property.h"

#include <gtest/gtest.h>

namespace
{
	struct FCapturedPropertyPathSegment
	{
		const Durin::FProperty* Property = nullptr;
		Durin::EPropertyPathSelector Selector = Durin::EPropertyPathSelector::None;
		uint64 Index = 0;
		Durin::FByteArray MapKeyData;
	};

	class DPropertyChangeObserver final : public Durin::DObject
	{
	public:
		auto PostEditChangeProperty(const Durin::FPropertyChangedEvent& Event) -> void override
		{
			++CallCount;
			MemberProperty = Event.MemberProperty;
			LeafProperty = Event.LeafProperty;
			Phase = Event.Phase;
			Kind = Event.Kind;
			Origin = Event.Origin;
			Path.clear();
			Path.reserve(Event.Path.size());
			for (const Durin::FPropertyPathSegment& Segment : Event.Path)
			{
				Path.push_back({
					Segment.Property,
					Segment.Selector,
					Segment.Index,
					Durin::FByteArray(Segment.MapKeyData.begin(), Segment.MapKeyData.end())
				});
			}
		}

		uint32 CallCount = 0;
		const Durin::FProperty* MemberProperty = nullptr;
		const Durin::FProperty* LeafProperty = nullptr;
		Durin::EPropertyChangePhase Phase = Durin::EPropertyChangePhase::Committed;
		Durin::EPropertyChangeKind Kind = Durin::EPropertyChangeKind::ValueSet;
		Durin::EPropertyChangeOrigin Origin = Durin::EPropertyChangeOrigin::Edit;
		std::vector<FCapturedPropertyPathSegment> Path;
	};

	struct FPropertyToken
	{
		alignas(Durin::FProperty) std::array<std::byte, sizeof(Durin::FProperty)> Storage{};

		auto Get() const -> const Durin::FProperty*
		{
			// Property-change delivery treats metadata as an opaque identity. Using
			// aligned tokens keeps this unit test independent of global FName and
			// reflected-type initialization owned by the reflection test fixture.
			return reinterpret_cast<const Durin::FProperty*>(Storage.data());
		}
	};
}

TEST(FPropertyChangeEventTests, VirtualHookReceivesScalarPath)
{
	const FPropertyToken ValueProperty;
	const std::array Path{Durin::FPropertyPathSegment{ValueProperty.Get()}};
	const Durin::FPropertyChangedEvent Event{
		ValueProperty.Get(),
		ValueProperty.Get(),
		Path,
		Durin::EPropertyChangePhase::Committed,
		Durin::EPropertyChangeKind::ValueSet,
		Durin::EPropertyChangeOrigin::Edit
	};

	DPropertyChangeObserver Observer;
	Durin::DObject* Object = &Observer;
	Object->PostEditChangeProperty(Event);

	EXPECT_EQ(Observer.CallCount, 1u);
	EXPECT_EQ(Observer.MemberProperty, ValueProperty.Get());
	EXPECT_EQ(Observer.LeafProperty, ValueProperty.Get());
	EXPECT_EQ(Observer.Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_EQ(Observer.Kind, Durin::EPropertyChangeKind::ValueSet);
	EXPECT_EQ(Observer.Origin, Durin::EPropertyChangeOrigin::Edit);
	ASSERT_EQ(Observer.Path.size(), 1u);
	EXPECT_EQ(Observer.Path[0].Property, ValueProperty.Get());
	EXPECT_EQ(Observer.Path[0].Selector, Durin::EPropertyPathSelector::None);
}

TEST(FPropertyChangeEventTests, PreservesNestedContainerSelectors)
{
	const FPropertyToken MapProperty;
	const FPropertyToken ArrayProperty;
	const FPropertyToken LeafProperty;
	const std::array<std::byte, 4> MapKeyData{
		std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
	const std::array Path{
		Durin::FPropertyPathSegment{MapProperty.Get(), Durin::EPropertyPathSelector::MapKey, 0, MapKeyData},
		Durin::FPropertyPathSegment{ArrayProperty.Get(), Durin::EPropertyPathSelector::ArrayIndex, 4},
		Durin::FPropertyPathSegment{LeafProperty.Get()}
	};
	const Durin::FPropertyChangedEvent Event{
		MapProperty.Get(),
		LeafProperty.Get(),
		Path,
		Durin::EPropertyChangePhase::Interactive,
		Durin::EPropertyChangeKind::ValueSet,
		Durin::EPropertyChangeOrigin::Redo
	};

	DPropertyChangeObserver Observer;
	Observer.PostEditChangeProperty(Event);

	EXPECT_EQ(Observer.MemberProperty, MapProperty.Get());
	EXPECT_EQ(Observer.LeafProperty, LeafProperty.Get());
	EXPECT_EQ(Observer.Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Observer.Origin, Durin::EPropertyChangeOrigin::Redo);
	ASSERT_EQ(Observer.Path.size(), 3u);
	EXPECT_EQ(Observer.Path[0].Selector, Durin::EPropertyPathSelector::MapKey);
	EXPECT_EQ(Observer.Path[0].MapKeyData, Durin::FByteArray(MapKeyData.begin(), MapKeyData.end()));
	EXPECT_EQ(Observer.Path[1].Selector, Durin::EPropertyPathSelector::ArrayIndex);
	EXPECT_EQ(Observer.Path[1].Index, 4u);
	EXPECT_EQ(Observer.Path[2].Property, LeafProperty.Get());
	EXPECT_EQ(Observer.Path[2].Selector, Durin::EPropertyPathSelector::None);
}
