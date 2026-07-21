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
} // namespace Durin
