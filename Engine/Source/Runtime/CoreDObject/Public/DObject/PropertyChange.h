#pragma once

#include "CoreMinimal.h"

#include "DObjectGlobals.h"
#include "DObject/ObjectValidation.h"

namespace Durin
{
	class FProperty;

	namespace EPropertyChangeType
	{
		using Type = uint32;
		inline constexpr Type ArrayAdd = 1 << 1;
		inline constexpr Type ArrayRemove = 1 << 2;
		inline constexpr Type ValueSet = 1 << 4;
		inline constexpr Type Interactive = 1 << 6;
	}

	namespace PropertyEditor::EPropertyValueSetFlags
	{
		using Type = uint32;
		inline constexpr Type DefaultFlags = 0;
		inline constexpr Type NotTransactable = 1 << 0;
		inline constexpr Type InteractiveChange = 1 << 1;
	}

	// Identifies whether an edit originated directly or through transaction history.
	enum class EPropertyChangeOrigin : uint8
	{
		Edit,
		Undo,
		Redo,
	};

	// Selects one nested value while traversing a reflected property path.
	enum class EPropertyPathSelector : uint8
	{
		None,
		StaticArrayIndex,
		ArrayIndex,
		MapKey,
	};

	// Describes one borrowed step from an object-owned member toward an edited leaf.
	struct FPropertyPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		// Index is meaningful only for fixed and dynamic array selectors. Map keys
		// use serialized bytes because an unordered-map iteration index is unstable.
		uint64 Index = 0;
		FByteView MapKeyData;
	};

	// Reports a completed reflected edit using synchronous borrowed path storage.
	struct FPropertyChangedEvent
	{
		const FProperty* MemberProperty = nullptr;
		const FProperty* LeafProperty = nullptr;
		// Paths run from the object-owned member to the edited leaf. The path and
		// map-key byte spans are borrowed for this synchronous notification only.
		std::span<const FPropertyPathSegment> Path;
		EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet;
		EPropertyChangeOrigin Origin = EPropertyChangeOrigin::Edit;
	};

	// A synchronous, detached candidate passed before reflected storage changes.
	// The containers are borrowed only for the hook call. Implementations may
	// normalize draft storage or reject it, but must not mutate the live object.
	// A removed or renamed map leaf may no longer exist in the candidate, in
	// which case DraftLeafContainer is null and the complete draft root remains.
	struct FPropertyEditProposal
	{
		const FProperty* MemberProperty = nullptr;
		const FProperty* LeafProperty = nullptr;
		PropertyEditor::EPropertyValueSetFlags::Type Flags = PropertyEditor::EPropertyValueSetFlags::DefaultFlags;
		const FProperty* DraftRootProperty = nullptr;
		void* DraftRootContainer = nullptr;
		uint32 DraftRootArrayIndex = 0;
		void* DraftLeafContainer = nullptr;
		uint32 DraftLeafArrayIndex = 0;
	};
} // namespace Durin
