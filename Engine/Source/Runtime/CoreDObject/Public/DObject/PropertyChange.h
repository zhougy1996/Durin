#pragma once

#include "DObjectGlobals.h"

namespace Durin
{
	class FProperty;

	enum class EPropertyChangePhase : uint8
	{
		Interactive,
		Committed,
		Cancelled,
	};

	enum class EPropertyChangeKind : uint8
	{
		ValueSet,
		ArrayAdd,
		ArrayRemove,
		ArrayResize,
		MapInsert,
		MapRemove,
		MapKeyRename,
	};

	enum class EPropertyChangeOrigin : uint8
	{
		Edit,
		Undo,
		Redo,
	};

	enum class EPropertyPathSelector : uint8
	{
		None,
		StaticArrayIndex,
		ArrayIndex,
		MapKey,
	};

	struct FPropertyPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		// Index is meaningful only for fixed and dynamic array selectors. Map keys
		// use serialized bytes because an unordered-map iteration index is unstable.
		uint64 Index = 0;
		std::span<const uint8> MapKeyData;
	};

	struct FPropertyChangedEvent
	{
		const FProperty* MemberProperty = nullptr;
		const FProperty* LeafProperty = nullptr;
		// Paths run from the object-owned member to the edited leaf. The path and
		// map-key byte spans are borrowed for this synchronous notification only.
		std::span<const FPropertyPathSegment> Path;
		EPropertyChangePhase Phase = EPropertyChangePhase::Committed;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;
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
		std::span<const FPropertyPathSegment> Path;
		EPropertyChangePhase Phase = EPropertyChangePhase::Interactive;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;
		EPropertyChangeOrigin Origin = EPropertyChangeOrigin::Edit;
		const FProperty* DraftRootProperty = nullptr;
		void* DraftRootContainer = nullptr;
		uint32 DraftRootArrayIndex = 0;
		void* DraftLeafContainer = nullptr;
		uint32 DraftLeafArrayIndex = 0;
	};
} // namespace Durin
