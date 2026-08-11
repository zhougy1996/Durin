#include "MonaImGuiWidgets.h"

#include "MonaImGui.h"

namespace Durin::MonaImGui
{
	namespace
	{
		auto ResizeStringInput(ImGuiInputTextCallbackData* Data) -> int
		{
			auto* Value = static_cast<std::string*>(Data->UserData);
			check(Data->EventFlag == ImGuiInputTextFlags_CallbackResize);
			check(Data->Buf == Value->data());
			Value->resize(static_cast<size_t>(Data->BufTextLen));
			Data->Buf = Value->data();
			return 0;
		}

		auto GetCompactTreeNodePadding() -> ImVec2
		{
			const ImVec2 FramePadding = ImGui::GetStyle().FramePadding;
			return {std::min(FramePadding.x, ScaleUI(2.0f)), FramePadding.y};
		}
	} // namespace

	namespace
	{
		auto DrawToolbarIconButton(const char* Icon, const char* Id, const char* Tooltip, float Width) -> bool
		{
			ImGui::PushID(Id);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
			const float Extent = ImGui::GetFrameHeight();
			const bool bPressed = ImGui::Button(Icon, ImVec2(Width, Extent));
			ImGui::PopStyleColor(3);
			ImGui::PopID();
			if (Tooltip != nullptr && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tooltip);
			return bPressed;
		}
	} // namespace

	auto ToolbarIconButton(const char* Icon, const char* Id, const char* Tooltip) -> bool
	{
		return DrawToolbarIconButton(Icon, Id, Tooltip, ImGui::GetFrameHeight());
	}

	auto CompactToolbarIconButton(const char* Icon, const char* Id, const char* Tooltip) -> bool
	{
		return DrawToolbarIconButton(Icon, Id, Tooltip, GetCompactToolbarIconButtonWidth());
	}

	auto GetCompactToolbarIconButtonWidth() -> float
	{
		return ScaleUI(28.0f);
	}

	auto DialogButton(const char* Label, bool bCompact) -> bool
	{
		const FUIStyleMetrics Metrics = GetUIStyleMetrics();
		return ImGui::Button(Label, ImVec2(bCompact ? Metrics.CompactButtonWidth : Metrics.StandardButtonWidth, 0.0f));
	}

	auto ErrorDialog(const char* Title, std::string& Message) -> void
	{
		if (!Title || Message.empty()) return;
		ImGui::OpenPopup(Title);

		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const float AvailableWidth = Viewport ? Viewport->WorkSize.x : ScaleUI(480.0f);
		const float DialogWidth = std::clamp(
			ScaleUI(440.0f),
			std::min(ScaleUI(260.0f), AvailableWidth),
			std::max(ScaleUI(260.0f), AvailableWidth - ScaleUI(48.0f)));
		const float MaximumHeight = Viewport
			? std::max(ScaleUI(160.0f), Viewport->WorkSize.y * 0.75f)
			: ScaleUI(560.0f);
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(DialogWidth, 0.0f),
			ImVec2(DialogWidth, MaximumHeight));
		if (!ImGui::BeginPopupModal(Title, nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;

		ImGui::TextWrapped("%s", Message.c_str());
		if (DialogButton("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			Message.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	auto InputText(const char* Label, std::string& Value, ImGuiInputTextFlags Flags) -> bool
	{
		check((Flags & ImGuiInputTextFlags_CallbackResize) == 0);
		return ImGui::InputText(
			Label,
			Value.data(),
			Value.capacity() + 1,
			Flags | ImGuiInputTextFlags_CallbackResize,
			ResizeStringInput,
			&Value
		);
	}

	auto CompactTreeNode(const char* Label, ImGuiTreeNodeFlags Flags) -> bool
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, GetCompactTreeNodePadding());
		const bool bOpen = ImGui::TreeNodeEx(Label, Flags);
		ImGui::PopStyleVar();
		return bOpen;
	}

	auto CompactTreeNode(const char* Id, ImGuiTreeNodeFlags Flags, const char* Format, ...) -> bool
	{
		va_list Args;
		va_start(Args, Format);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, GetCompactTreeNodePadding());
		const bool bOpen = ImGui::TreeNodeExV(Id, Flags, Format, Args);
		ImGui::PopStyleVar();
		va_end(Args);
		return bOpen;
	}

	auto GetCompactTreeNodeToLabelSpacing() -> float
	{
		return ImGui::GetFontSize() + GetCompactTreeNodePadding().x * 2.0f;
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
