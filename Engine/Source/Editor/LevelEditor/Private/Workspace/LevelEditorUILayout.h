#pragma once

namespace Durin
{
	// Selects the default docking arrangement for the level editor.
	enum class EEditorUILayoutMode : uint8
	{
		Narrow,
		Compact,
		Full,
	};

	constexpr auto ResolveEditorUILayout(float AvailableWidth, float CompactMinimumWidth, float FullMinimumWidth) -> EEditorUILayoutMode
	{
		if (AvailableWidth >= FullMinimumWidth) return EEditorUILayoutMode::Full;
		if (AvailableWidth >= CompactMinimumWidth) return EEditorUILayoutMode::Compact;
		return EEditorUILayoutMode::Narrow;
	}
} // namespace Durin
