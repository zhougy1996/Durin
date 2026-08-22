#pragma once

#include "DObjectGlobals.h"

namespace Durin
{
	class FProperty;
	using FPropertyEditDeferredCompletion = std::function<void(bool, std::string)>;
	using FPropertyEditDeferredCancel = std::function<void()>;
	using FPropertyEditDeferredAction = std::function<
		FPropertyEditDeferredCancel(FPropertyEditDeferredCompletion)>;

	// Distinguishes preview, accepted, and abandoned reflected edits.
	enum class EPropertyChangePhase : uint8
	{
		Interactive,
		Committed,
		Cancelled,
	};

	// Identifies the structural operation performed at the edited property leaf.
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
		std::span<const std::byte> MapKeyData;
	};

	// Reports a completed reflected edit using synchronous borrowed path storage.
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

		// Defers publication until detached asynchronous validation completes.
		FPropertyEditDeferredAction DeferredAction;
		auto Defer(FPropertyEditDeferredAction Action) -> bool
		{
			if (!Action || DeferredAction) return false;
			DeferredAction = std::move(Action);
			return true;
		}
	};
} // namespace Durin
