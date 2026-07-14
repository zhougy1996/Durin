#pragma once

#include "DObject/Class.h"
#include "MonaImGui.h"

namespace Durin::LevelEditorHelpers
{
	inline auto ClassDisplayName(const DClass* Class) -> std::string
	{
		const std::string Name = Class ? Class->GetName() : std::string();
		const size_t Separator = Name.rfind("::");
		return Separator == std::string::npos ? Name : Name.substr(Separator + 2);
	}

	inline auto DrawToolbarIconButton(const char* Icon, const char* Id) -> bool
	{
		ImGui::PushID(Id);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
		const bool bPressed = ImGui::Button(Icon, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
		ImGui::PopStyleColor(3);
		ImGui::PopID();
		return bPressed;
	}
} // namespace Durin::LevelEditorHelpers
