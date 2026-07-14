#include "MonaImGuiWidgets.h"

#include "MonaImGui.h"

namespace Durin::MonaImGui
{
	auto ToolbarIconButton(const char* Icon, const char* Id, const char* Tooltip) -> bool
	{
		ImGui::PushID(Id);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
		const float Extent = ImGui::GetFrameHeight();
		const bool bPressed = ImGui::Button(Icon, ImVec2(Extent, Extent));
		ImGui::PopStyleColor(3);
		ImGui::PopID();
		if (Tooltip != nullptr && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tooltip);
		return bPressed;
	}

	auto DialogButton(const char* Label, bool bCompact) -> bool
	{
		const FUIStyleMetrics Metrics = GetUIStyleMetrics();
		return ImGui::Button(Label, ImVec2(bCompact ? Metrics.CompactButtonWidth : Metrics.StandardButtonWidth, 0.0f));
	}

	auto DrawSplitter(
		const char* Id,
		EUISplitterAxis Axis,
		float Length,
		float TotalSize,
		float MinimumFirstSize,
		float MinimumSecondSize,
		float& Ratio
	) -> bool
	{
		const float Thickness = GetUIStyleMetrics().SplitterThickness;
		const ImVec2 Size = Axis == EUISplitterAxis::X ? ImVec2(Thickness, Length) : ImVec2(Length, Thickness);
		ImGui::InvisibleButton(Id, Size);

		const bool bActive = ImGui::IsItemActive();
		const bool bHovered = ImGui::IsItemHovered();
		if (bActive || bHovered) ImGui::SetMouseCursor(Axis == EUISplitterAxis::X ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		const ImGuiCol Color = bActive ? ImGuiCol_SeparatorActive : bHovered ? ImGuiCol_SeparatorHovered :
																			   ImGuiCol_Separator;
		ImGui::GetWindowDrawList()->AddRectFilled(Min, Max, ImGui::GetColorU32(Color));

		if (!bActive || TotalSize <= 0.0f) return false;
		const float Delta = Axis == EUISplitterAxis::X ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
		const float FirstSize = std::clamp(Ratio * TotalSize + Delta, MinimumFirstSize, std::max(MinimumFirstSize, TotalSize - MinimumSecondSize));
		const float NewRatio = std::clamp(FirstSize / TotalSize, 0.0f, 1.0f);
		const bool bChanged = NewRatio != Ratio;
		Ratio = NewRatio;
		return bChanged;
	}
} // namespace Durin::MonaImGui
