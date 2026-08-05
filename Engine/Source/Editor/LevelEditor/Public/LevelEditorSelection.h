#pragma once

#include "Misc/Guid.h"

namespace Durin
{
	enum class EEditorSubElementKind : uint8
	{
		None,
		Point,
		Segment,
		ArriveTangent,
		LeaveTangent,
	};

	// Identifies a component-owned editor element without relying on an unstable array index.
	struct FEditorSubElementSelection
	{
		EEditorSubElementKind Kind = EEditorSubElementKind::None;
		FGuid StableId;
		int32 SecondaryIndex = INDEX_NONE;

		auto IsValid() const -> bool { return Kind != EEditorSubElementKind::None && StableId.IsValid(); }
		auto operator==(const FEditorSubElementSelection&) const -> bool = default;
	};
} // namespace Durin
