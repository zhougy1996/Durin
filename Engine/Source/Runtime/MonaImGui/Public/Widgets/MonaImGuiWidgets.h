#pragma once

#include "MonaImGuiAPI.h"
#include "ImGui/ImGuiCommon.h"

namespace Durin
{
	class FName;
}

namespace Durin::MonaImGui
{
	// Draws the full display name, including any numeric suffix, as one unformatted text item.
	// Unnumbered names borrow pool storage without a string copy; numbered names use a stack
	// buffer. Explicit text bounds avoid requiring null-terminated pool storage.
	// Requires an active ImGui window, just like TextUnformatted(); this is text, not a widget label/ID.
	MONAIMGUI_API auto TextName(const FName& Name) -> void;

	// Selects the screen-space axis along which a splitter divides content.
	enum class EUISplitterAxis : uint8
	{
		X,
		Y,
	};

	MONAIMGUI_API auto ToolbarIconButton(const char* Icon, const char* Id, const char* Tooltip = nullptr) -> bool;
	// Draws an asset-field action button with a narrower footprint while retaining the row height.
	MONAIMGUI_API auto CompactToolbarIconButton(const char* Icon, const char* Id, const char* Tooltip = nullptr) -> bool;
	MONAIMGUI_API auto GetCompactToolbarIconButtonWidth() -> float;
	MONAIMGUI_API auto DialogButton(const char* Label, bool bCompact = false) -> bool;
	// Begins a themed, contiguous tab bar for switching between content views.
	// Call EndContentTabBar only when this returns true.
	MONAIMGUI_API auto BeginContentTabBar(
		const char* Id,
		ImGuiTabBarFlags Flags = ImGuiTabBarFlags_None
	) -> bool;
	MONAIMGUI_API auto EndContentTabBar() -> void;
	// Draws a consistently sized modal and clears Message when the user dismisses it.
	MONAIMGUI_API auto ErrorDialog(const char* Title, std::string& Message) -> void;
	MONAIMGUI_API auto InputText(const char* Label, std::string& Value, ImGuiInputTextFlags Flags = ImGuiInputTextFlags_None) -> bool;
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
