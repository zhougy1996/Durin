#pragma once

#include "CoreMinimal.h"

namespace Durin::LevelEditor
{
	enum class EEditorSubElementKind : uint8
	{
		None,
		Point,
		Segment,
		ArriveTangent,
		LeaveTangent,
	};

	// Identifies a component-owned editor element by its index in the current topology.
	struct FEditorSubElementSelection
	{
		EEditorSubElementKind Kind = EEditorSubElementKind::None;
		int32 Index = INDEX_NONE;
		uint64 TopologyRevision = 0;

		auto IsValid() const -> bool { return Kind != EEditorSubElementKind::None && Index >= 0; }
		auto operator==(const FEditorSubElementSelection&) const -> bool = default;
	};
} // namespace Durin::LevelEditor
