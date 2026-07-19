#pragma once

#include "MonaImGuiAPI.h"
#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Durin::MonaImGui
{
	enum class EUISplitterAxis : uint8
	{
		X,
		Y,
	};

	MONAIMGUI_API auto ToolbarIconButton(const char* Icon, const char* Id, const char* Tooltip = nullptr) -> bool;
	MONAIMGUI_API auto DialogButton(const char* Label, bool bCompact = false) -> bool;
	MONAIMGUI_API auto CompactTreeNode(const char* Label, ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_None) -> bool;
	MONAIMGUI_API auto CompactTreeNode(const char* Id, ImGuiTreeNodeFlags Flags, const char* Format, ...) -> bool;
	MONAIMGUI_API auto GetCompactTreeNodeToLabelSpacing() -> float;
	MONAIMGUI_API auto DrawSplitter(
		const char* Id,
		EUISplitterAxis Axis,
		float Length,
		float TotalSize,
		float MinimumFirstSize,
		float MinimumSecondSize,
		float& Ratio
	) -> bool;
} // namespace Durin::MonaImGui
